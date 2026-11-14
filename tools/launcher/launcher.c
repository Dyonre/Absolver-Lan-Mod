// AbsolverModLauncher - starts Absolver's game exe directly with -NoEAC so proxy-DLL mods (the loader's version.dll etc.) load.
// Why: Steam's Play button runs Absolver.exe, the EasyAntiCheat launcher, which refuses modded DLLs. The game exe has its own
// offline switch (-NoEAC); this program just uses it. It never renames, moves or edits any EAC or game file.
// Use: double-click it in the game folder, or Steam > Properties > Launch options:  "<path>\AbsolverModLauncher.exe" %command%
// Flags: --setup (copy the Steam launch option to the clipboard), --migrate (move non-vanilla paks into Mods\ and exit), -NoMods (passed to the game; the mod loader honours it).
// It also works installed AS Absolver.exe (what the installer does): Steam's Play button then starts the modded game with no launch option.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <shellapi.h>
#include "args.h"

#define APPID "473690"
#define GAME_EXE L"Absolver-Win64-Shipping.exe"
#define REL_BIN L"Absolver\\Binaries\\Win64"

static wchar_t g_log[MAX_PATH];
static void logf_(const wchar_t *fmt, ...) {
    if (!g_log[0]) return;
    wchar_t msg[1200]; va_list ap; va_start(ap, fmt); _vsnwprintf(msg, 1199, fmt, ap); va_end(ap); msg[1199] = 0;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t line[1400]; int n = swprintf(line, 1400, L"[%04d-%02d-%02d %02d:%02d:%02d] launcher: %ls\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
    char u8[4200]; int b = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof u8, NULL, NULL);
    HANDLE h = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, u8, b, &w, NULL); CloseHandle(h); }
}
static void fail(const wchar_t *text) { logf_(L"ERROR: %ls", text); MessageBoxW(NULL, text, L"Absolver Mod Launcher", MB_OK | MB_ICONERROR); }
static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// A game root is a folder that contains Absolver\Binaries\Win64\Absolver-Win64-Shipping.exe.
static int is_game_root(const wchar_t *root) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\%ls\\%ls", root, REL_BIN, GAME_EXE); return exists(p);
}
static int steam_running(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return 1;   // cannot tell: assume yes
    PROCESSENTRY32W e = { sizeof e }; int found = 0;
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e)) if (!_wcsicmp(e.szExeFile, L"steam.exe")) { found = 1; break; }
    CloseHandle(s); return found;
}
// Fallback when the launcher is not inside the game folder: SteamPath + libraryfolders.vdf ("path" entries).
static int find_in_steam(wchar_t *root) {
    wchar_t steam[MAX_PATH]; DWORD sz = sizeof steam; HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    LONG r = RegQueryValueExW(k, L"SteamPath", NULL, NULL, (BYTE *)steam, &sz); RegCloseKey(k); if (r != ERROR_SUCCESS) return 0;
    for (wchar_t *c = steam; *c; c++) if (*c == L'/') *c = L'\\';
    wchar_t cand[MAX_PATH]; swprintf(cand, MAX_PATH, L"%ls\\steamapps\\common\\Absolver", steam);
    if (is_game_root(cand)) { wcscpy(root, cand); return 1; }
    wchar_t vdf[MAX_PATH]; swprintf(vdf, MAX_PATH, L"%ls\\steamapps\\libraryfolders.vdf", steam);
    FILE *f = _wfopen(vdf, L"rb"); if (!f) return 0;
    char line[1024]; int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = strstr(line, "\"path\""); if (!p) continue;
        p = strchr(p + 6, '"'); if (!p) continue; char *q = strchr(++p, '"'); if (!q) continue; *q = 0;
        wchar_t lib[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, p, -1, lib, MAX_PATH);
        wchar_t clean[MAX_PATH]; int j = 0;                                   // vdf escapes backslashes as "\\"
        for (int i = 0; lib[i] && j < MAX_PATH - 1; i++) { if (lib[i] == L'\\' && lib[i + 1] == L'\\') i++; clean[j++] = lib[i]; }
        clean[j] = 0;
        swprintf(cand, MAX_PATH, L"%ls\\steamapps\\common\\Absolver", clean);
        if (is_game_root(cand)) { wcscpy(root, cand); found = 1; }
    }
    fclose(f); return found;
}
static void copy_to_clipboard(const wchar_t *text) {
    size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes); if (!g) return;
    memcpy(GlobalLock(g), text, bytes); GlobalUnlock(g);
    if (OpenClipboard(NULL)) { EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); } else GlobalFree(g);
}

// <game>\Mods: where every non-vanilla pak lives. Created here (and by the DLL) so the folder is there before the first mod is added.
static const char MODS_README[] =
    "Absolver mods folder\r\n"
    "====================\r\n\r\n"
    "Put each mod in its own folder:   Mods\\<ModName>\\<ModName>.pak\r\n"
    "(a .sig file is not needed; the loader creates one next to the pak).\r\n\r\n"
    "Load order is Mods\\loadorder.txt, which the loader creates and keeps up to date. The LAST line wins when two mods change the same file.\r\n"
    "Put a - in front of a name to switch that mod off. LanNative\\mods.txt (written every start) lists conflicts between your mods.\r\n\r\n"
    "Only the game's own pakchunk0/1/2 files stay in Absolver\\Content\\Paks; the launcher moves other paks here automatically.\r\n"
    "Start the game with -NoMods to run without any of them.\r\n";
static void ensure_mods_dir(const wchar_t *root) {
    wchar_t mods[MAX_PATH], readme[MAX_PATH];
    swprintf(mods, MAX_PATH, L"%ls\\Mods", root); swprintf(readme, MAX_PATH, L"%ls\\README.txt", mods);
    if (CreateDirectoryW(mods, NULL)) logf_(L"created %ls", mods);
    if (!exists(readme)) {
        HANDLE h = CreateFileW(readme, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, MODS_README, (DWORD)strlen(MODS_README), &w, NULL); CloseHandle(h); }
    }
}
// Moves every non-vanilla .pak (+ its .sig) from Absolver\Content\Paks into Mods\<Name>\ . Same drive, so a rename: nothing is copied or deleted.
// Returns the number of paks moved. Subfolders (LogicMods for UE4SS) are left alone.
static int migrate_foreign_paks(const wchar_t *root) {
    wchar_t paks[MAX_PATH], pat[MAX_PATH]; int moved = 0;
    swprintf(paks, MAX_PATH, L"%ls\\Absolver\\Content\\Paks", root); swprintf(pat, MAX_PATH, L"%ls\\*.pak", paks);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    wchar_t names[64][MAX_PATH]; int n = 0;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !is_vanilla_pak_name(fd.cFileName) && n < 64) wcscpy(names[n++], fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (int i = 0; i < n; i++) {                       // ascending name = the order the engine ranks paks in Paks (higher name wins), so loadorder.txt keeps the same winners
        for (int j = i + 1; j < n; j++) if (_wcsicmp(names[j], names[i]) < 0) { wchar_t t[MAX_PATH]; wcscpy(t, names[i]); wcscpy(names[i], names[j]); wcscpy(names[j], t); }
    }
    for (int i = 0; i < n; i++) {
        wchar_t folder[MAX_PATH], dir[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
        if (!mod_folder_name(names[i], folder, MAX_PATH)) { logf_(L"migrate: cannot name a folder for %ls (skipped)", names[i]); continue; }
        swprintf(dir, MAX_PATH, L"%ls\\Mods\\%ls", root, folder);
        for (int k = 2; exists(dir) && k < 50; k++) swprintf(dir, MAX_PATH, L"%ls\\Mods\\%ls_%d", root, folder, k);
        if (!CreateDirectoryW(dir, NULL)) { logf_(L"migrate: cannot create %ls (error %lu)", dir, GetLastError()); continue; }
        swprintf(src, MAX_PATH, L"%ls\\%ls", paks, names[i]); swprintf(dst, MAX_PATH, L"%ls\\%ls", dir, names[i]);
        if (!MoveFileExW(src, dst, 0)) { logf_(L"migrate: could not move %ls (error %lu; is the game running?)", names[i], GetLastError()); RemoveDirectoryW(dir); continue; }
        wchar_t sigsrc[MAX_PATH], sigdst[MAX_PATH]; wcscpy(sigsrc, src); wcscpy(sigdst, dst);
        size_t a = wcslen(sigsrc), b = wcslen(sigdst); if (a > 4) wcscpy(sigsrc + a - 4, L".sig"); if (b > 4) wcscpy(sigdst + b - 4, L".sig");
        if (exists(sigsrc)) MoveFileExW(sigsrc, sigdst, 0);
        logf_(L"migrate: %ls -> Mods\\%ls\\", names[i], wcsrchr(dir, L'\\') + 1); moved++;
    }
    return moved;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy(dir, self); wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *sl = 0;

    wchar_t root[MAX_PATH] = L"";
    if (is_game_root(dir)) wcscpy(root, dir);
    else if (!find_in_steam(root)) { fail(L"Could not find the Absolver game folder. Put AbsolverModLauncher.exe in the game's main folder (next to Absolver.exe)."); return 2; }

    wchar_t bin[MAX_PATH], exe[MAX_PATH], appid[MAX_PATH], logdir[MAX_PATH];
    swprintf(bin, MAX_PATH, L"%ls\\%ls", root, REL_BIN);
    swprintf(exe, MAX_PATH, L"%ls\\%ls", bin, GAME_EXE);
    swprintf(appid, MAX_PATH, L"%ls\\steam_appid.txt", bin);
    swprintf(logdir, MAX_PATH, L"%ls\\LanNative", bin);
    CreateDirectoryW(logdir, NULL);
    swprintf(g_log, MAX_PATH, L"%ls\\launcher.log", logdir);
    logf_(L"start: self=%ls root=%ls args=%d", self, root, argc - 1);
    ensure_mods_dir(root);
    int migrated = migrate_foreign_paks(root);
    if (has_arg(argc, argv, L"--migrate")) {       // tidy up and exit without starting the game
        logf_(L"migrate: done, %d pak(s) moved", migrated); return 0;
    }

    if (has_arg(argc, argv, L"--setup")) {
        wchar_t opt[MAX_PATH + 40]; swprintf(opt, MAX_PATH + 40, L"\"%ls\\AbsolverModLauncher.exe\" %%command%%", root);
        copy_to_clipboard(opt);
        logf_(L"setup: launch option copied: %ls", opt);
        wchar_t msg[MAX_PATH + 400];
        swprintf(msg, MAX_PATH + 400, L"The Steam launch option was copied to the clipboard:\n\n%ls\n\nIn Steam: right-click Absolver > Properties > General > Launch Options > paste.\nThen the normal Play button starts the modded game.\nTo play online again, clear that box.", opt);
        MessageBoxW(NULL, msg, L"Absolver Mod Launcher - setup", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!steam_running()) { fail(L"Steam is not running. Start Steam first, then run this launcher again."); return 3; }
    if (!exists(appid)) {   // without it the game exe hands off to Steam, which starts the EAC launcher
        HANDLE h = CreateFileW(appid, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) { fail(L"Could not create steam_appid.txt in the game's Binaries\\Win64 folder (is the game folder read-only?)."); return 4; }
        DWORD w; WriteFile(h, APPID, (DWORD)strlen(APPID), &w, NULL); CloseHandle(h); logf_(L"created steam_appid.txt");
    }
    wchar_t dll[MAX_PATH]; swprintf(dll, MAX_PATH, L"%ls\\version.dll", bin);
    logf_(L"version.dll (LanNative proxy) %ls", exists(dll) ? L"present" : L"NOT present - the game will start without mods");

    wchar_t cl[4096];
    if (build_game_cmdline(cl, 4096, exe, argc, argv) < 0) { fail(L"Command line too long."); return 5; }
    logf_(L"starting: %ls", cl);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, bin, &si, &pi)) {
        wchar_t m[200]; swprintf(m, 200, L"Could not start the game (Windows error %lu).", GetLastError()); fail(m); return 6;
    }
    DWORD t0 = GetTickCount();
    WaitForSingleObject(pi.hProcess, INFINITE);   // stay alive so Steam Cloud / playtime see a normal session
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DWORD secs = (GetTickCount() - t0) / 1000;
    logf_(L"game exited: code 0x%08lX after %lu s", code, secs);
    if (secs < 10 && code != 0) {
        wchar_t m[600]; swprintf(m, 600, L"The game closed after %lu seconds (exit code 0x%08lX).\n\nSee LanNative\\launcher.log and LanNative\\lannative.log in the game's Binaries\\Win64 folder. Try starting with -NoMods.", secs, code);
        MessageBoxW(NULL, m, L"Absolver Mod Launcher", MB_OK | MB_ICONWARNING);
    }
    return 0;
}
