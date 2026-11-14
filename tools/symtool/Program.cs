using System.Runtime.InteropServices;
using System.Text;

// Static stand-in for a debugger, driven by the game's shipped PDB via Windows' DbgHelp.
//   symtool <exe> find  <mask...>          list matching symbols (RVA, size)
//   symtool <exe> calls <mask...>          for each matching function: direct calls, import calls,
//                                          and RIP-relative references (with the string they point at)
//   symtool <exe> xref  <fileOffsetHex>    which functions reference that file offset (lea/mov rip)
//   symtool <exe> callers <mask...>        every direct call/jmp site that targets a matching symbol

const ulong BASE = 0x140000000;
var exePath = args[0];
var mode = args[1];
var exe = File.ReadAllBytes(exePath);

int peOff = BitConverter.ToInt32(exe, 0x3C);
int numSections = BitConverter.ToUInt16(exe, peOff + 6);
int optSize = BitConverter.ToUInt16(exe, peOff + 20);
uint sizeOfImage = BitConverter.ToUInt32(exe, peOff + 24 + 56);
int secTable = peOff + 24 + optSize;
var sections = new List<(string name, uint va, int rawPtr, int rawSize, bool exec)>();
for (int s = 0; s < numSections; s++)
{
    int e = secTable + s * 40;
    sections.Add((Encoding.ASCII.GetString(exe, e, 8).TrimEnd('\0'), BitConverter.ToUInt32(exe, e + 12),
        BitConverter.ToInt32(exe, e + 20), BitConverter.ToInt32(exe, e + 16),
        (BitConverter.ToUInt32(exe, e + 36) & 0x20000000) != 0));
}
int RvaToFile(uint rva)
{
    foreach (var s in sections) if (rva >= s.va && rva < s.va + (uint)s.rawSize) return (int)(rva - s.va) + s.rawPtr;
    return -1;
}
uint FileToRva(int fo)
{
    foreach (var s in sections) if (fo >= s.rawPtr && fo < s.rawPtr + s.rawSize) return s.va + (uint)(fo - s.rawPtr);
    throw new ArgumentException($"file offset 0x{fo:X} not inside any section");
}

var h = (IntPtr)0x5A5A;
SymSetOptions(0x2 | 0x200);
if (!SymInitializeW(h, Path.GetDirectoryName(exePath), false)) { Console.WriteLine($"SymInitialize failed {Marshal.GetLastWin32Error()}"); return 1; }
SymLoadModuleExW(h, IntPtr.Zero, exePath, null, BASE, sizeOfImage, IntPtr.Zero, 0);

var symBuf = Marshal.AllocHGlobal(88 + 2 * 2048);
string NameAt(uint rva, out ulong disp)
{
    Marshal.WriteInt32(symBuf, 0, 88);
    Marshal.WriteInt32(symBuf, 80, 2000);
    if (!SymFromAddrW(h, BASE + rva, out disp, symBuf)) { disp = 0; return "?"; }
    return Marshal.PtrToStringUni(symBuf + 84, Marshal.ReadInt32(symBuf, 76)).TrimEnd('\0');
}

string? StrAt(uint rva)
{
    int fo = RvaToFile(rva);
    if (fo < 0) return null;
    int i = fo;
    while (i < exe.Length && i - fo < 160 && exe[i] >= 0x20 && exe[i] < 0x7F) i++;
    if (i - fo >= 3 && i < exe.Length && exe[i] == 0) return $"\"{Encoding.ASCII.GetString(exe, fo, i - fo)}\"";
    int j = fo;
    while (j + 1 < exe.Length && (j - fo) / 2 < 160 && exe[j] >= 0x20 && exe[j] < 0x7F && exe[j + 1] == 0) j += 2;
    if ((j - fo) / 2 >= 2 && j + 1 < exe.Length && exe[j] == 0 && exe[j + 1] == 0) return $"L\"{Encoding.Unicode.GetString(exe, fo, j - fo)}\"";
    return null;
}

List<(uint rva, int size, string name)> Enum(string mask)
{
    var list = new List<(uint, int, string)>();
    SymEnumCallback cb = (p, _, _) =>
    {
        var name = Marshal.PtrToStringUni(p + 84, Marshal.ReadInt32(p, 76)).TrimEnd('\0');
        list.Add(((uint)((ulong)Marshal.ReadInt64(p, 56) - BASE), Marshal.ReadInt32(p, 28), name));
        return true;
    };
    SymEnumSymbolsW(h, BASE, mask, cb, IntPtr.Zero);
    GC.KeepAlive(cb);
    return list.DistinctBy(x => x.Item1).ToList();
}

IEnumerable<(string kind, uint target, int len)> Decode(int fo, uint rva)
{
    byte b0 = exe[fo];
    if (b0 is 0xE8 or 0xE9) yield return (b0 == 0xE8 ? "call" : "jmp ", (uint)(rva + 5 + BitConverter.ToInt32(exe, fo + 1)), 5);
    if (b0 == 0xFF && (exe[fo + 1] is 0x15 or 0x25)) yield return (exe[fo + 1] == 0x15 ? "call*" : "jmp* ", (uint)(rva + 6 + BitConverter.ToInt32(exe, fo + 2)), 6);
    if ((b0 is 0x48 or 0x4C) && (exe[fo + 1] is 0x8D or 0x8B) && (exe[fo + 2] & 0xC7) == 0x05)
    {
        string[] regs = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi" };
        int reg = (exe[fo + 2] >> 3) & 7;
        string r = b0 == 0x4C ? $"r{reg + 8}" : regs[reg];
        yield return ($"{(exe[fo + 1] == 0x8D ? "lea" : "mov")} {r}", (uint)(rva + 7 + BitConverter.ToInt32(exe, fo + 3)), 7);
    }
}

switch (mode)
{
    case "dumpall":                                          // every symbol with a size: "rva size name" lines (no 300 limit) - input of tools\lannative\gen_symbols.py
        foreach (var (rva, size, name) in Enum("*"))
            if (size > 0) Console.WriteLine($"{rva:X8} {size} {name}");
        break;

    case "find":
        foreach (var mask in args.Skip(2))
            foreach (var (rva, size, name) in Enum(mask).Take(300))
                Console.WriteLine($"  RVA 0x{rva:X8} size {size,6}  {name}");
        break;

    case "calls":
        foreach (var mask in args.Skip(2))
        {
            foreach (var (rva, size, name) in Enum(mask).Where(x => x.size > 0).Take(40))
            {
                Console.WriteLine($"\n== {name}  RVA 0x{rva:X8} size {size}");
                int fo0 = RvaToFile(rva);
                for (int k = 0; k < size - 5; k++)
                {
                    foreach (var (kind, target, len) in Decode(fo0 + k, rva + (uint)k))
                    {
                        var tname = NameAt(target, out var tdisp);
                        var str = StrAt(target);
                        bool isCall = kind.StartsWith("call") || kind.StartsWith("jmp");
                        if (isCall && (tdisp != 0 || tname == "?")) continue;
                        if (isCall && kind.StartsWith("jmp") && target >= rva && target < rva + size) continue;
                        if (!isCall && str == null && tname == "?") continue;
                        Console.WriteLine($"  +0x{k:X4} {kind,-9} {(str ?? $"{tname}{(tdisp != 0 ? $"+0x{tdisp:X}" : "")}")}");
                    }
                }
            }
        }
        break;

    case "resolve":
        // resolve <file>: one hex RVA per line in, "RVA<TAB>name+disp" out (used by tools/disasm/disasm.py).
        foreach (var line in File.ReadLines(args[2]))
        {
            if (!uint.TryParse(line.Trim(), System.Globalization.NumberStyles.HexNumber, null, out var r)) continue;
            var n = NameAt(r, out var dd);
            Console.WriteLine($"{r:X8}\t{(n == "?" ? "?" : n + (dd != 0 ? $"+0x{dd:X}" : ""))}");
        }
        break;

    case "callers":
    {
        var targets = new Dictionary<uint, string>();
        foreach (var mask in args.Skip(2)) foreach (var (rva, _, name) in Enum(mask)) targets.TryAdd(rva, name);
        Console.WriteLine($"{targets.Count} target symbol(s)");
        foreach (var s in sections.Where(s => s.exec))
        {
            int end = Math.Min(s.rawPtr + s.rawSize, exe.Length) - 5;
            for (int i = s.rawPtr; i < end; i++)
            {
                if (exe[i] != 0xE8 && exe[i] != 0xE9) continue;
                uint rva = s.va + (uint)(i - s.rawPtr);
                uint target = (uint)(rva + 5 + BitConverter.ToInt32(exe, i + 1));
                if (targets.TryGetValue(target, out var tname))
                    Console.WriteLine($"  {(exe[i] == 0xE8 ? "call" : "jmp ")} {tname}  from {NameAt(rva, out var d)}+0x{d:X}");
            }
        }
        break;
    }

    case "xref":
    {
        int fo = Convert.ToInt32(args[2], 16);
        uint want = FileToRva(fo);
        Console.WriteLine($"file offset 0x{fo:X} = RVA 0x{want:X8} ({NameAt(want, out var d0)}+0x{d0:X}) {StrAt(want)}");
        foreach (var s in sections.Where(s => s.exec))
        {
            int end = Math.Min(s.rawPtr + s.rawSize, exe.Length) - 7;
            for (int i = s.rawPtr; i < end; i++)
            {
                uint rva = s.va + (uint)(i - s.rawPtr);
                foreach (var (kind, target, _) in Decode(i, rva))
                    if (target >= want && target < want + 64 && !kind.StartsWith("call") && !kind.StartsWith("jmp"))
                        Console.WriteLine($"  {kind} at RVA 0x{rva:X8} in {NameAt(rva, out var d)}+0x{d:X}  (-> +{target - want})");
            }
        }
        break;
    }
}
return 0;

[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Unicode)]
static extern bool SymInitializeW(IntPtr hProcess, string? userSearchPath, bool invadeProcess);
[DllImport("dbghelp.dll", SetLastError = true)]
static extern uint SymSetOptions(uint options);
[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Unicode)]
static extern ulong SymLoadModuleExW(IntPtr hProcess, IntPtr hFile, string imageName, string? moduleName, ulong baseOfDll, uint dllSize, IntPtr data, uint flags);
[DllImport("dbghelp.dll", SetLastError = true, CharSet = CharSet.Unicode)]
static extern bool SymEnumSymbolsW(IntPtr hProcess, ulong baseOfDll, string mask, SymEnumCallback callback, IntPtr userContext);
[DllImport("dbghelp.dll", SetLastError = true)]
static extern bool SymFromAddrW(IntPtr hProcess, ulong address, out ulong displacement, IntPtr symbol);

delegate bool SymEnumCallback(IntPtr symInfo, uint symbolSize, IntPtr userContext);
