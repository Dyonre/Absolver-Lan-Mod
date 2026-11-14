r"""gen_symbols.py - symbol table for hook_symbol(): name -> RVA + original bytes + stolen length, from the game's PDB (via tools\symtool dumpall) and the exe.
  python gen_symbols.py <symbols_all.txt> [<out symbols.bin>]      (set ABSOLVER_EXE or ABSOLVER_GAME first)
  python gen_symbols.py --probe <rva hex> ...                       (stolen length and bytes of single functions)
Input  : symtool dumpall output ("RVA size name" per line).
Output : symbols.bin  = "ASYM1\0\0\0" | u32 count | count x { u64 fnv1a64(name), u32 rva, u8 stolen, u8 bytes[24] } sorted by hash (binary search at runtime)
         symbols_check.txt = validation report. The file is derived from the game (names, bytes): keep it local, never commit it.
stolen = the shortest instruction-aligned prefix >= 14 bytes that contains no relative branch/call and no RIP-relative operand (those cannot be copied into
a trampoline), at most 24 bytes; 0 = the function cannot be hooked with a 14-byte jump (too short or position dependent prologue).
Self-check: the hand-verified loader targets (lannative_targets.h) must come out with the same bytes; stolen may differ only where the hand pick was longer."""
import sys, struct, re, os, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

HERE = os.path.dirname(os.path.abspath(__file__))
USAGE = ("usage: python gen_symbols.py <symbols_all.txt> [<out symbols.bin>]\n"
         "       python gen_symbols.py --probe <rva hex> ...\n"
         "symbols_all.txt is the output of: symtool <path to Absolver-Win64-Shipping.exe> dumpall\n"
         "The game exe is found through ABSOLVER_EXE (full path) or ABSOLVER_GAME (the folder that holds Absolver.exe).")
def find_exe():
    p = os.environ.get("ABSOLVER_EXE") or (os.path.join(os.environ["ABSOLVER_GAME"], "Absolver", "Binaries", "Win64", "Absolver-Win64-Shipping.exe") if os.environ.get("ABSOLVER_GAME") else "")
    if not p or not os.path.isfile(p): sys.exit(USAGE + "\nAbsolver-Win64-Shipping.exe not found: set ABSOLVER_EXE or ABSOLVER_GAME.")
    return p
if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"): sys.exit(USAGE)
EXE = find_exe()
src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "symbols.bin")

def fnv1a64(s):
    h = 0xcbf29ce484222325
    for c in s.encode("utf-8"):
        h ^= c; h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

pe = pefile.PE(EXE); img = open(EXE, "rb").read()
def file_off(rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize: return rva - s.VirtualAddress + s.PointerToRawData
    return None
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def stolen_len(rva, size):
    fo = file_off(rva)
    if fo is None or size < 14: return 0, b""
    code = img[fo:fo + 40]
    n = 0
    for ins in md.disasm(code, rva):
        if ins.group(1) or ins.group(7) or ins.group(2) or ins.group(3) or ins.group(5):   # jump/call/ret/int/iret groups (capstone: JUMP=1 CALL=2 RET=3 INT=4 IRET=5)
            return 0, b""
        if ins.mnemonic in ("jmp", "call", "ret", "int3", "ud2") or ins.mnemonic.startswith("j") or ins.mnemonic.startswith("loop"):
            return 0, b""
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP: return 0, b""
        n += ins.size
        if n >= 14: break
    if n < 14 or n > 24 or n > size: return 0, b""
    return n, code[:n]

if len(sys.argv) > 1 and sys.argv[1] == "--probe":
    for a in sys.argv[2:]:
        st, b = stolen_len(int(a, 16), 4096); print(a, st, b.hex(" "))
    sys.exit(0)
syms = {}                                   # hash -> (rva, stolen, bytes, name)
dup = skipped = 0
for line in open(src, encoding="utf-8", errors="replace"):
    m = re.match(r"([0-9A-Fa-f]{8}) (\d+) (.+)", line.strip())
    if not m: continue
    rva, size, name = int(m.group(1), 16), int(m.group(2)), m.group(3)
    if "`" in name or name.startswith("??") or "<lambda" in name: continue
    st, b = stolen_len(rva, size)
    if not st: skipped += 1; continue
    h = fnv1a64(name)
    if h in syms and syms[h][0] != rva: dup += 1; continue                          # overloads share a name: keep the first, report
    syms[h] = (rva, st, b, name)
entries = sorted(syms.items())
with open(out, "wb") as f:
    f.write(b"ASYM1\0\0\0"); f.write(struct.pack("<I", len(entries)))
    for h, (rva, st, b, name) in entries:
        f.write(struct.pack("<QIB", h, rva, st)); f.write(b.ljust(24, b"\0"))
print("symbols hookable:", len(entries), " skipped (short / relative prologue):", skipped, " same-name overloads dropped:", dup, " ->", out, os.path.getsize(out), "bytes")

# self-check against the hand-verified loader targets
tg = open(os.path.join(HERE, "lannative_targets.h"), encoding="utf-8").read()
rep = []
bad = 0
for m in re.finditer(r'\{ "(\w+)", 0x([0-9A-F]+), (\d+), \{ ([^}]*) \} \}', tg):
    name, rva, st = m.group(1), int(m.group(2), 16), int(m.group(3)); bs = bytes(int(x, 16) for x in m.group(4).split(","))
    mine_st, mine_b = stolen_len(rva, 4096)
    ok = mine_st == st and mine_b == bs
    pref = mine_b[:len(bs)] == bs[:len(mine_b)] if mine_st else False
    rep.append("%-60s hand=%2d gen=%2d %s" % (name, st, mine_st, "SAME" if ok else ("prefix-ok (hand pick longer or generator stricter)" if pref else "DIFFERENT")))
    if not (ok or pref): bad += 1
open(os.path.join(HERE, "symbols_check.txt"), "w").write("\n".join(rep) + "\n")
print("\n".join(rep)); print("self-check problems:", bad)


