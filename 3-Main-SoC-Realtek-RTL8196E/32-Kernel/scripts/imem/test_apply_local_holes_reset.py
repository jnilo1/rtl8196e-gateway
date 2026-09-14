#!/usr/bin/env python3
"""Regression test: --reset must leave a restored object OLDER than a source
edited after the last make (otherwise make never recompiles that source).
Host-only, no toolchain: --reset never parses ELF."""
import os, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
tool = os.path.join(here, "apply_local_holes.py")
with tempfile.TemporaryDirectory() as build:
    os.makedirs(os.path.join(build, "net/core"))
    obj = os.path.join(build, "net/core/skbuff.o")
    src = os.path.join(build, "net/core/skbuff.c")
    pristine = obj + ".imem-pristine"
    old = time.time() - 3600
    for f in (pristine, obj):
        open(f, "wb").write(b"ELF-not-really")
        os.utime(f, (old, old))
    open(src, "w").write("/* edited after the last make */\n")   # mtime = now
    policy = os.path.join(build, "policy.tsv")
    open(policy, "w").write("# test policy\nnet/core/skbuff.o\t.text.kmalloc_reserve\n")
    subprocess.run([sys.executable, tool, "--policy", policy, "--build-dir", build, "--reset"],
                   check=True, capture_output=True)
    m_obj, m_src, m_pristine = (os.stat(f).st_mtime for f in (obj, src, pristine))
    assert m_obj == m_pristine, f"restored object mtime {m_obj} != pristine {m_pristine}"
    assert m_obj < m_src, f"restored object ({m_obj}) is not older than the edited source ({m_src})"
print("test_apply_local_holes_reset: OK — restored object keeps the pristine mtime and stays older than an edited source")
