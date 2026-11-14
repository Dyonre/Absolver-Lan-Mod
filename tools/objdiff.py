"""objdiff.py <vanilla objdump.txt> <modded objdump.txt> [more pairs...] > object-edits.json

Turns two LanNative object dumps (F11 command  objdump <objpath>  with a mod off and on) into the "object" edits of a rows.json: for every property whose text differs, the
smallest set / append ops that turn the vanilla text into the modded text. Arrays that only grew (the usual case: a mod adds a style or an animation) become `append` ops
at the right path, so several mods can grow the same array. Anything it cannot express (an element removed, a member that does not exist in vanilla) is reported and skipped.

The text grammar and the path syntax are the ones in tools/lannative/uetext.h:  structs (name=value,...), arrays (v0,v1,...), static array members name[i]=value,
paths like  m_Array[0].m_Array  (a trailing .[i] picks element i of an array reached through an indexed member).
"""
import json, sys

class Node:
    __slots__ = ("is_list", "s", "e", "children", "name")
    def __init__(self): self.is_list = False; self.s = 0; self.e = 0; self.children = []; self.name = None

def ident(c): return c.isalnum() or c in "_[]"

def leaf_end(t, p):
    depth = 0; n = len(t)
    while p < n:
        c = t[p]
        if c == '"':
            p += 1
            while p < n and t[p] != '"':
                if t[p] == '\\' and p + 1 < n: p += 1
                p += 1
            p += 1; continue
        if c == "'":
            p += 1
            while p < n and t[p] != "'": p += 1
            p += 1; continue
        if c == '(': depth += 1
        elif c == ')':
            if depth == 0: return p
            depth -= 1
        elif c == ',' and depth == 0: return p
        p += 1
    return p

def parse_value(t, pos, depth=0):
    if depth > 64: raise ValueError("nested too deeply")
    n = len(t)
    while pos < n and t[pos] in " \t\r\n": pos += 1
    node = Node(); start = pos
    if pos < n and t[pos] == '(':
        node.is_list = True; node.s = start; pos += 1
        while True:
            while pos < n and t[pos] in " \t\r\n": pos += 1
            if pos >= n: raise ValueError("missing ')'")
            if t[pos] == ')': pos += 1; break
            ns = pos; ne = pos
            while ne < n and ident(t[ne]): ne += 1
            named = ne > ns and ne < n and t[ne] == '='
            if named: pos = ne + 1
            child, pos = parse_value(t, pos, depth + 1)
            if named: child.name = t[ns:ne]
            node.children.append(child)
            while pos < n and t[pos] in " \t\r\n": pos += 1
            if pos < n and t[pos] == ',': pos += 1
        node.e = pos; return node, pos
    e = leaf_end(t, pos); tr = e
    while tr > start and t[tr - 1] in " \t": tr -= 1
    node.s = start; node.e = tr; return node, e

def parse(t):
    node, pos = parse_value(t, 0)
    if t[pos:].strip(): raise ValueError("extra text after the value")
    return node

def join(path, seg):
    return seg if not path else path + "." + seg

def elem_path(path, i):
    last = path.rsplit(".", 1)[-1]
    return path + (".[%d]" % i if "[" in last or not path else "[%d]" % i)

def diff(tv, nv, ts, ns, path, ops, problems):
    if not nv.is_list or not ns.is_list:
        if tv[nv.s:nv.e] != ts[ns.s:ns.e]: ops.append(("set", path, ts[ns.s:ns.e]))
        return
    named = any(c.name for c in ns.children) or any(c.name for c in nv.children)
    if named:
        vm = {c.name: c for c in nv.children}
        for c in ns.children:
            if c.name not in vm: problems.append("%s: new member %s" % (path, c.name)); continue
            diff(tv, vm[c.name], ts, c, join(path, c.name), ops, problems)
        for name in vm:
            if name not in {c.name for c in ns.children}: problems.append("%s: member %s was removed" % (path, name))
        return
    vc, sc = nv.children, ns.children
    if len(sc) < len(vc): problems.append("%s: %d element(s) removed" % (path, len(vc) - len(sc))); return
    for i in range(len(vc)):
        diff(tv, vc[i], ts, sc[i], elem_path(path, i), ops, problems)
    for i in range(len(vc), len(sc)):
        ops.append(("append", path, ts[sc[i].s:sc[i].e]))

def load(p):
    props = {}; header = ""
    for line in open(p, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("# "): header = line[2:].split("  (class")[0]
        elif " = " in line and " : " in line.split(" = ", 1)[0]:
            k, v = line.split(" = ", 1); props[k.split(" : ")[0]] = v
    return header, props

def main(argv):
    edits = []; total = 0
    for i in range(0, len(argv) - 1, 2):
        header, pv = load(argv[i]); _, pm = load(argv[i + 1])
        ops = []; problems = []
        for name, mv in pm.items():
            if name not in pv or pv[name] == mv: continue
            vv = pv[name]
            try:
                if vv.startswith("(") and mv.startswith("("):
                    diff(vv, parse(vv), mv, parse(mv), name, ops, problems)
                else:
                    ops.append(("set", name, mv))
            except ValueError as e:
                problems.append("%s: %s" % (name, e))
        for p in problems: print("WARNING %s: %s" % (header, p), file=sys.stderr)
        if ops:
            edits.append({"object": header, "ops": [{k: path, "value": val} for k, path, val in ops]}); total += len(ops)
    json.dump({"format": 1, "edits": edits}, sys.stdout, indent=1)
    print("%d ops for %d object(s)" % (total, len(edits)), file=sys.stderr)

if __name__ == "__main__":
    if len(sys.argv) < 3 or len(sys.argv) % 2 == 0: print(__doc__); sys.exit(2)
    main(sys.argv[1:])
