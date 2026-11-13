#!/usr/bin/env python3
"""Lists the imports of a PE that a UWP app on Xbox may not have.

Allowed = every function WindowsApp.lib (the UWP umbrella lib) exports, matched by name (MinGW links kernel32.dll,
WindowsApp.lib names the api-ms-win-* sets; both resolve to the same exports), plus api-ms-win-crt-* (UCRT) and the
DLLs that ship inside the package (--packaged).

usage: check_uwp_imports.py <exe-or-dll> [--packaged a.dll,b.dll] [--llvm <bin dir>]
"""
import argparse
import os
import re
import subprocess
import sys

SDK_LIB = r"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64\WindowsApp.lib"


def allowed_names(llvm):
    out = subprocess.run([os.path.join(llvm, "llvm-nm"), "-A", SDK_LIB], capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        m = re.match(r".*:\s*[0-9a-fA-F]*\s+T\s+(\S+)$", line)
        if m and not m.group(1).startswith("__imp_"):
            names.add(m.group(1))
    return names


def imports(llvm, path):
    out = subprocess.run([os.path.join(llvm, "llvm-readobj"), "--coff-imports", path], capture_output=True,
                         text=True).stdout
    dll, result = None, []
    for line in out.splitlines():
        m = re.match(r"\s*Name: (\S+\.dll)", line, re.I)
        if m:
            dll = m.group(1)
            continue
        m = re.match(r"\s*Symbol: (\S+)", line)
        if m and dll:
            result.append((dll, m.group(1)))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pe")
    ap.add_argument("--packaged", default="")
    ap.add_argument("--llvm", default=os.path.join(os.path.dirname(__file__), "llvm-mingw", "bin"))
    a = ap.parse_args()
    packaged = {d.strip().lower() for d in a.packaged.split(",") if d.strip()}
    ok = allowed_names(a.llvm)
    bad = []
    for dll, sym in imports(a.llvm, a.pe):
        d = dll.lower()
        if d in packaged or d.startswith("api-ms-win-crt-") or d == "ucrtbase.dll":
            continue
        if sym not in ok:
            bad.append((dll, sym))
    for dll, sym in bad:
        print(f"NOT-UWP {dll}!{sym}")
    print(f"{len(bad)} import(s) outside the UWP API set")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
