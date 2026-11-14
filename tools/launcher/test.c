#include <stdio.h>
#include "args.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void cmd(const wchar_t *expect, int argc, wchar_t **argv) {
    wchar_t buf[512]; int n = build_game_cmdline(buf, 512, L"C:\\g\\Shipping.exe", argc, argv);
    if (n < 0 || wcscmp(buf, expect)) { wprintf(L"FAIL: got [%ls] want [%ls]\n", buf, expect); fails++; }
}
int main(void) {
    CHECK(is_steam_command_path(L"<game>\\Absolver.exe"));
    CHECK(is_steam_command_path(L"\"C:\\x\\absolver.EXE\""));
    CHECK(is_steam_command_path(L"Absolver.exe"));
    CHECK(!is_steam_command_path(L"C:\\x\\NotAbsolver.exe"));
    CHECK(!is_steam_command_path(L"Absolver-Win64-Shipping.exe"));
    CHECK(!is_steam_command_path(L"-NoMods"));
    wchar_t *a1[] = { L"l.exe" };                                        cmd(L"\"C:\\g\\Shipping.exe\" -NoEAC", 1, a1);
    wchar_t *a2[] = { L"l.exe", L"C:\\s\\Absolver.exe", L"-windowed" };  cmd(L"\"C:\\g\\Shipping.exe\" -NoEAC -windowed", 3, a2);
    wchar_t *a3[] = { L"l.exe", L"-NoEAC", L"-NoMods" };                 cmd(L"\"C:\\g\\Shipping.exe\" -NoEAC -NoMods", 3, a3);
    wchar_t *a4[] = { L"l.exe", L"-windowed", L"C:\\s\\Absolver.exe" };  cmd(L"\"C:\\g\\Shipping.exe\" -NoEAC -windowed C:\\s\\Absolver.exe", 3, a4);  /* only argv[1] is %command% */
    wchar_t *a5[] = { L"l.exe", L"--setup", L"a b" };                    cmd(L"\"C:\\g\\Shipping.exe\" -NoEAC \"a b\"", 3, a5);
    wchar_t small[10]; CHECK(build_game_cmdline(small, 10, L"C:\\g\\Shipping.exe", 1, a1) < 0);
    CHECK(has_arg(3, a3, L"-nomods")); CHECK(!has_arg(2, a3, L"-nomods"));
    CHECK(is_vanilla_pak_name(L"pakchunk0-WindowsNoEditor.pak")); CHECK(is_vanilla_pak_name(L"PAKCHUNK2-windowsnoeditor.sig"));
    CHECK(!is_vanilla_pak_name(L"pakchunk3-HarderDownfall1.5.3.pak")); CHECK(!is_vanilla_pak_name(L"pakchunk3-WindowsNoEditor.pak"));
    CHECK(!is_vanilla_pak_name(L"zzzProxyMod.pak")); CHECK(!is_vanilla_pak_name(L"pakchunk0-WindowsNoEditor.pak.bak"));
    wchar_t nm[64];
    CHECK(mod_folder_name(L"zzzProxyMod.pak", nm, 64) && !wcscmp(nm, L"ProxyMod"));
    CHECK(mod_folder_name(L"pakchunk3-HarderDownfall1.5.3.pak", nm, 64) && !wcscmp(nm, L"pakchunk3-HarderDownfall1.5.3"));
    CHECK(mod_folder_name(L"zzzOtherModV1.1.pak", nm, 64) && !wcscmp(nm, L"OtherModV1.1"));
    CHECK(mod_folder_name(L"zzz.pak", nm, 64) && !wcscmp(nm, L"zzz"));          /* "zzz" alone keeps its name */
    CHECK(mod_folder_name(L"a:b*c.pak", nm, 64) && !wcscmp(nm, L"a_b_c"));
    CHECK(!mod_folder_name(L".pak", nm, 64)); CHECK(!mod_folder_name(L"toolong.pak", nm, 4));
    CHECK(mod_folder_name(L"ends.dots...pak", nm, 64) && !wcscmp(nm, L"ends.dots"));
    printf(fails ? "FAILED %d\n" : "all launcher arg checks passed\n", fails);
    return fails != 0;
}
