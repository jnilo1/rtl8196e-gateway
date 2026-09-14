#!/usr/bin/env python3
"""Move selected ELF input sections to I-MEM while retaining local holes.

The original section header, size, alignment and position are retained and its
contents become MIPS NOPs.  A duplicate section receives the real code,
symbols and relocation target.  A generated global root keeps the symbol-free
hole when the kernel links with --gc-sections; the relink must pass one
``-u __imem_hole_NNNN`` option per selected section.
"""

import argparse
import json
import os
import shutil
import struct

ELF_HEADER = ">16sHHIIIIIHHHHHH"
SECTION_HEADER = ">IIIIIIIIII"
SYMBOL = ">IIIBBH"
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_REL = 9
SHT_PROGBITS = 1
SHF_GROUP = 0x200


def aligned(value, alignment):
    alignment = max(1, alignment)
    return (value + alignment - 1) // alignment * alignment


def cstring(data, offset):
    end = data.index(0, offset)
    return bytes(data[offset:end]).decode("ascii")


def move_section(path, old_name, new_name):
    raw = bytearray(open(path, "rb").read())
    fields = list(struct.unpack_from(ELF_HEADER, raw, 0))
    ident = fields[0]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 2:
        raise SystemExit(f"{path}: expected ELF32 big-endian object")
    shoff, shentsize, shnum, shstrndx = fields[6], fields[11], fields[12], fields[13]
    if shentsize != struct.calcsize(SECTION_HEADER):
        raise SystemExit(f"{path}: unexpected section-header size {shentsize}")
    headers = [list(struct.unpack_from(SECTION_HEADER, raw, shoff + i * shentsize))
               for i in range(shnum)]
    shstr = headers[shstrndx]
    names = raw[shstr[4]:shstr[4] + shstr[5]]
    indices = [index for index, header in enumerate(headers)
               if cstring(names, header[0]) == old_name]
    if len(indices) != 1:
        raise SystemExit(f"{path}: {old_name}: expected one section, found {len(indices)}")
    old_index = indices[0]
    old = headers[old_index]
    if old[1] != SHT_PROGBITS or not old[5]:
        raise SystemExit(f"{path}: {old_name}: not a non-empty PROGBITS section")
    if old[2] & SHF_GROUP:
        raise SystemExit(f"{path}: {old_name}: grouped sections are unsupported")

    code = bytes(raw[old[4]:old[4] + old[5]])
    raw[old[4]:old[4] + old[5]] = b"\0" * old[5]
    new_index = shnum

    moved_symbols = 0
    for header in headers:
        if header[1] != SHT_SYMTAB or not header[9]:
            continue
        count = header[5] // header[9]
        for index in range(count):
            offset = header[4] + index * header[9]
            symbol = list(struct.unpack_from(SYMBOL, raw, offset))
            if symbol[5] == old_index:
                symbol[5] = new_index
                struct.pack_into(SYMBOL, raw, offset, *symbol)
                moved_symbols += 1
    if not moved_symbols:
        raise SystemExit(f"{path}: {old_name}: no defining symbol was moved")

    symtabs = [(index, header) for index, header in enumerate(headers)
               if header[1] == SHT_SYMTAB]
    if len(symtabs) != 1:
        raise SystemExit(f"{path}: expected one symbol table, found {len(symtabs)}")
    symtab_index, symtab = symtabs[0]
    symstr_index = symtab[6]
    symstr = headers[symstr_index]
    root_name = "__imem_hole_" + new_name.rsplit(".", 1)[-1]
    symstr_data = bytes(raw[symstr[4]:symstr[4] + symstr[5]])
    root_name_offset = len(symstr_data)
    symstr_data += root_name.encode("ascii") + b"\0"
    symtab_data = bytes(raw[symtab[4]:symtab[4] + symtab[5]])
    # A global root is appended after the local-symbol range (sh_info).
    symtab_data += struct.pack(SYMBOL, root_name_offset, 0, 0, 0x10, 0, old_index)

    moved_relocations = 0
    for header in headers:
        if header[1] in (SHT_REL, SHT_RELA) and header[7] == old_index:
            header[7] = new_index
            moved_relocations += 1

    new_names = bytes(names) + new_name.encode("ascii") + b"\0"
    new_name_offset = len(names)
    cursor = len(raw)
    code_offset = aligned(cursor, old[8])
    raw.extend(b"\0" * (code_offset - len(raw)))
    raw.extend(code)
    symstr_offset = len(raw)
    raw.extend(symstr_data)
    symtab_offset = aligned(len(raw), 4)
    raw.extend(b"\0" * (symtab_offset - len(raw)))
    raw.extend(symtab_data)
    headers[symstr_index][4] = symstr_offset
    headers[symstr_index][5] = len(symstr_data)
    headers[symtab_index][4] = symtab_offset
    headers[symtab_index][5] = len(symtab_data)
    names_offset = len(raw)
    raw.extend(new_names)
    headers[shstrndx][4] = names_offset
    headers[shstrndx][5] = len(new_names)

    new_header = old.copy()
    new_header[0] = new_name_offset
    new_header[3] = 0
    new_header[4] = code_offset
    headers.append(new_header)

    new_shoff = aligned(len(raw), 4)
    raw.extend(b"\0" * (new_shoff - len(raw)))
    for header in headers:
        raw.extend(struct.pack(SECTION_HEADER, *header))
    fields[6] = new_shoff
    fields[12] = len(headers)
    struct.pack_into(ELF_HEADER, raw, 0, *fields)

    with open(path, "wb") as stream:
        stream.write(raw)
    os.utime(path, None)
    return moved_symbols, moved_relocations, root_name, old[5], old[8]


def load_selection(manifest_path, policy_path):
    if manifest_path:
        with open(manifest_path, encoding="utf-8") as stream:
            manifest = json.load(stream)
        selected = [item for item in manifest["candidates"]
                    if item.get("selected")]
        selected.sort(key=lambda value: (value["address"], value["object"],
                                         value["section"]))
        return selected, manifest["budget"]

    selected = []
    with open(policy_path, encoding="utf-8") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 2 or not all(fields):
                raise SystemExit(f"{policy_path}:{line_number}: expected object<TAB>section")
            selected.append({"object": fields[0], "section": fields[1]})
    keys = [(item["object"], item["section"]) for item in selected]
    if not selected:
        raise SystemExit(f"{policy_path}: empty I-MEM policy")
    if len(keys) != len(set(keys)):
        raise SystemExit(f"{policy_path}: duplicate object/section entry")
    return selected, 15872


def main():
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--manifest")
    source.add_argument("--policy")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--report")
    args = parser.parse_args()
    selected, budget = load_selection(args.manifest, args.policy)
    build = os.path.abspath(args.build_dir)

    if args.reset:
        restored = 0
        for item in selected:
            path = os.path.join(build, item["object"])
            pristine = path + ".imem-pristine"
            if os.path.exists(pristine):
                # copy2 keeps the pristine object's mtime: the restored .o must
                # stay OLDER than a source edited since the last make, or make
                # considers it up to date and never recompiles that source.
                shutil.copy2(pristine, path)
                restored += 1
        print(f"restored {restored}/{len(selected)} selected objects")
        return

    # The caller restores before make, then reaches this point only after make
    # has rebuilt anything made stale by a source change. Refresh the pristine
    # copy unconditionally: retaining an older backup here would silently undo
    # that recompilation before the I-MEM relink.
    for item in selected:
        path = os.path.join(build, item["object"])
        pristine = path + ".imem-pristine"
        shutil.copy2(path, pristine)

    total = 0
    report_entries = []
    for order, item in enumerate(selected):
        path = os.path.join(build, item["object"])
        name = f".iram.sel.{order:04d}"
        symbols, relocations, root, size, alignment = move_section(
            path, item["section"], name)
        total = aligned(total, alignment) + size
        report_entries.append({"order": order, "object": item["object"],
                               "section": item["section"], "size": size,
                               "alignment": alignment, "root": root})
        print(f"{order:4d} {item['section']:<42} {size:5d} B  "
              f"{symbols} symbols, {relocations} relocation sections, root {root}")
    if total > budget:
        raise SystemExit(f"linked-order estimate {total} exceeds {budget} bytes")
    if args.report:
        with open(args.report, "w", encoding="utf-8") as stream:
            json.dump({"schema": 1, "sections": len(selected),
                       "occupation": total, "budget": budget,
                       "entries": report_entries}, stream, indent=2)
            stream.write("\n")
    print(f"local-hole selection: {len(selected)} sections, {total} bytes")


if __name__ == "__main__":
    main()
