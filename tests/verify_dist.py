#!/usr/bin/env python3
"""
verify_dist.py - Smoke test for KVMapper Windows binaries.

Parses the PE header of each shipped .exe / .dll and asserts:
  - correct machine architecture (x64 vs x86)
  - GUI subsystem (not console)
  - required DLL imports are present

Per windows-native-cicd skill §3: this is the gate between "build
succeeded" and "ready to publish". Without this, CI was the only
fast-feedback loop and was happy to ship a build where the x86 exe
loaded a 64-bit DLL by name.

Run locally:
    python3 tests/verify_dist.py

Exit code 0 = all checks pass.
"""
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"

IMAGE_FILE_MACHINE_I386  = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664

SUBSYSTEM_GUI            = 2
SUBSYSTEM_CONSOLE        = 3

DIRECTORY_ENTRY_IMPORT   = 1


def parse_pe(path: Path):
    """Return dict { machine, subsystem, imports: set[str.upper()] }."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE (missing MZ)")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise ValueError(f"{path}: missing PE\\0\\0 signature")

    # COFF header (20 bytes) starts at e_lfanew + 4
    coff_off = e_lfanew + 4
    machine, num_sections, _timedate, _ptr_sym, _num_sym, opt_size, _chars = \
        struct.unpack_from("<HHIIIHH", data, coff_off)

    opt_off = coff_off + 20
    magic = struct.unpack_from("<H", data, opt_off)[0]
    is_pe32_plus = (magic == 0x20B)

    # Subsystem lives at fixed offsets in the optional header
    if is_pe32_plus:
        subsystem = struct.unpack_from("<H", data, opt_off + 68)[0]
        # NumberOfRvaAndSizes is at opt_off + 108 for PE32+
        num_rva_off = opt_off + 108
        first_dir_off = opt_off + 112
    else:
        subsystem = struct.unpack_from("<H", data, opt_off + 68)[0]
        num_rva_off = opt_off + 92
        first_dir_off = opt_off + 96

    num_rva = struct.unpack_from("<I", data, num_rva_off)[0]
    if num_rva <= DIRECTORY_ENTRY_IMPORT:
        return {"machine": machine, "subsystem": subsystem, "imports": set()}

    imp_rva, imp_size = struct.unpack_from(
        "<II", data, first_dir_off + DIRECTORY_ENTRY_IMPORT * 8
    )

    # Section table starts at opt_off + opt_size
    sec_off = opt_off + opt_size

    def rva_to_off(rva):
        for i in range(num_sections):
            s = sec_off + i * 40
            _name = data[s:s + 8]
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, s + 8)
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        return None

    imports = set()
    if imp_rva and imp_size:
        imp_off = rva_to_off(imp_rva)
        if imp_off is not None:
            idx = imp_off
            # IMAGE_IMPORT_DESCRIPTOR is 20 bytes; terminated by an all-zero row.
            while idx + 20 <= len(data):
                fields = struct.unpack_from("<IIIII", data, idx)
                if not any(fields):
                    break
                name_rva = fields[3]
                name_off = rva_to_off(name_rva)
                if name_off is not None:
                    end = data.index(b"\x00", name_off)
                    imports.add(data[name_off:end].decode("ascii", "replace").upper())
                idx += 20

    return {"machine": machine, "subsystem": subsystem, "imports": imports}


REQUIRED_EXE_IMPORTS = {"USER32.DLL", "KERNEL32.DLL", "SHELL32.DLL"}
REQUIRED_DLL_IMPORTS = {"USER32.DLL", "KERNEL32.DLL"}

EXPECTED = [
    # path,                              machine,                  required imports, subsystem
    ("kvmapper.exe",            IMAGE_FILE_MACHINE_AMD64, REQUIRED_EXE_IMPORTS, SUBSYSTEM_GUI),
    ("kvmapper_hook.dll",       IMAGE_FILE_MACHINE_AMD64, REQUIRED_DLL_IMPORTS, None),
    ("kvmapper_hook_x86.dll",   IMAGE_FILE_MACHINE_I386,  REQUIRED_DLL_IMPORTS, None),
]

# x86 exe is optional - present only if build-all.sh built it.
OPTIONAL = [
    ("kvmapper_x86.exe",        IMAGE_FILE_MACHINE_I386,  REQUIRED_EXE_IMPORTS, SUBSYSTEM_GUI),
]


def check(rel, expect_machine, expect_imports, expect_subsystem):
    path = DIST / rel
    if not path.exists():
        return False, f"MISSING: {rel}"
    info = parse_pe(path)
    if info["machine"] != expect_machine:
        return False, (f"{rel}: wrong machine 0x{info['machine']:X}, "
                       f"expected 0x{expect_machine:X}")
    if expect_subsystem is not None and info["subsystem"] != expect_subsystem:
        return False, (f"{rel}: wrong subsystem {info['subsystem']}, "
                       f"expected {expect_subsystem} (GUI)")
    missing = expect_imports - info["imports"]
    if missing:
        return False, f"{rel}: missing imports {sorted(missing)}"
    return True, f"OK {rel} (machine=0x{info['machine']:X}, imports={len(info['imports'])})"


def main():
    if not DIST.exists():
        print(f"FAIL: dist/ does not exist ({DIST})")
        return 1

    failed = 0
    for rel, mach, imps, sub in EXPECTED:
        ok, msg = check(rel, mach, imps, sub)
        print(("PASS  " if ok else "FAIL  ") + msg)
        if not ok:
            failed += 1

    for rel, mach, imps, sub in OPTIONAL:
        path = DIST / rel
        if not path.exists():
            print(f"SKIP  {rel} (optional, not built)")
            continue
        ok, msg = check(rel, mach, imps, sub)
        print(("PASS  " if ok else "FAIL  ") + msg)
        if not ok:
            failed += 1

    print()
    print(f"Failed: {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
