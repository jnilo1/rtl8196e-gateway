#!/usr/bin/env python3
"""Compare the preprocessed form of every translation unit between two trees.

Usage: tests/preproc_diff.py <build-log> <tree-A> <tree-B> [--show N]

<build-log> is the output of build_bootloader.sh (any tree): its
`mips-lexra-linux-musl-gcc ... -c ... -o build/x.o` lines give the exact flags
of every unit of both variants (production and RAMTEST_TRACE). Each line is
rerun with -E -P in <tree-A> and in <tree-B> (paths 31-Bootloader/), the outputs
are compared line by line, and the lines that changed form are printed. A
header clean-up is acceptable when every printed line is an equivalent
re-spelling (a redundant cast dropped, a constant re-spelled, a prototype
replacing an unused inline) — the binaries being identical says the code did
not change, this says *why* the preprocessed text did.

A line that is added at one place and removed at another in the same unit
has only moved (an include order changed): it is counted apart, per unit,
and does not fail the run.

Exit status 0 when nothing was added or changed, 1 otherwise (removed lines
are the point of a clean-up and are only counted; moved lines are listed).
"""
import sys, os, shlex, subprocess, difflib, collections

def units(log):
    for l in open(log):
        if l.startswith('mips-lexra-linux-musl-gcc') and ' -c ' in l and l.rstrip().endswith('.o'):
            yield l.strip()

def preprocess(cmd, tree):
    args = shlex.split(cmd)
    src = [a for a in args if a.endswith(('.c', '.S'))][0]
    cwd = 'btcode' if src in ('bootload.c', 'LzmaDecode.c', 'piggy.S', 'start.S') else 'boot'
    # -c goes (we preprocess only); so does -MD, which would write /dev/stdout.d
    args = [a for a in args if a not in ('-c', '-MD', '-MMD', '-MP')]
    args[args.index('-o') + 1] = '/dev/stdout'
    args[1:1] = ['-E', '-P']
    r = subprocess.run(args, cwd=os.path.join(tree, cwd), capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{src} in {tree}: {r.stderr.strip()[:200]}")
    return src, ('ramtest' if '-DRAMTEST_TRACE' in cmd else 'prod'), \
           [l.strip() for l in r.stdout.split('\n') if l.strip()]

def main():
    log, a, b = sys.argv[1:4]
    show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 40
    added = collections.Counter(); moved = collections.Counter(); removed = 0; n = 0
    for cmd in units(log):
        src, var, la = preprocess(cmd, a)
        _, _, lb = preprocess(cmd, b)
        n += 1
        plus = collections.Counter(); minus = collections.Counter()
        # autojunk off: with it, lines that recur often (`return 0;`, `}`) are
        # treated as junk and reported as spurious +/- pairs around big deletions
        sm = difflib.SequenceMatcher(None, la, lb, autojunk=False)
        for op, i1, i2, j1, j2 in sm.get_opcodes():
            if op in ('delete', 'replace'):
                minus.update(la[i1:i2])
            if op in ('insert', 'replace'):
                plus.update(lb[j1:j2])
        for l, k in plus.items():
            m = min(k, minus.get(l, 0))
            if m:
                moved[(src, var)] += m
            if k - m:
                added[(src, var, l)] += k - m
        removed += sum(minus.values())
    print(f"units: {n}; preprocessed lines removed: {removed}; "
          f"moved within a unit: {sum(moved.values())}; "
          f"added or changed: {sum(added.values())} ({len(added)} distinct)")
    for (src, var), k in sorted(moved.items()):
        print(f"  moved {k:3d}  {src} [{var}]")
    for (src, var, l), k in added.most_common(show):
        print(f"  {k:3d}  {src} [{var}]: {l[:140]}")
    return 1 if added else 0

if __name__ == '__main__':
    sys.exit(main())
