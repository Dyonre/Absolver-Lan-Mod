"""Scan a folder (a release staging dir or a zip) for text that identifies the person who built it.

usage: python scan_personal.py <folder-or-zip> [extra pattern ...]

Patterns checked, as UTF-8 and UTF-16 (how .NET stores string literals): this Windows user name, this computer name, the user's profile
path, plus anything given on the command line (pass a folder name or any other identifying text there). Exit code 1 if anything is found, so the release script can stop.
"""
import getpass, os, socket, sys, zipfile

def default_patterns():
    pats = {getpass.getuser(), socket.gethostname(), os.path.expanduser("~")}
    pats |= {p.lower() for p in list(pats)}
    return {p for p in pats if len(p) >= 4}

def scan_bytes(name, data, pats, hits):
    for p in pats:
        for enc in ("utf-8", "utf-16-le"):
            i = data.find(p.encode(enc))
            if i >= 0:
                ctx = data[max(0, i - 10): i + 70].decode(enc, "replace").replace(chr(0), "")
                ctx = "".join(c if c.isprintable() else "." for c in ctx)
                hits.setdefault(name, set()).add(f"{p} ({enc}): ...{ctx}...")

def main():
    target = sys.argv[1]
    pats = default_patterns() | set(sys.argv[2:])
    hits, n = {}, 0
    if zipfile.is_zipfile(target):
        with zipfile.ZipFile(target) as z:
            for i in z.infolist():
                if i.is_dir(): continue
                n += 1
                scan_bytes(i.filename, z.read(i), pats, hits)
                scan_bytes(i.filename, i.filename.encode(), pats, hits)   # the entry name itself
    else:
        for dp, _, fns in os.walk(target):
            for f in fns:
                p = os.path.join(dp, f)
                n += 1
                with open(p, "rb") as fh:
                    scan_bytes(os.path.relpath(p, target), fh.read(), pats, hits)
    print(f"scanned {n} files for {len(pats)} patterns")
    for f, v in sorted(hits.items()):
        print("  FOUND", f, sorted(v))
    if not hits: print("  nothing identifying found")
    return 1 if hits else 0

if __name__ == "__main__":
    sys.exit(main())
