// Safe mode, crash-loop guard and crash attribution (included by lannative.c before init()).
//
// Safe mode = the loader runs only its built-in features: no Mods\ (no paks, no rows.json, no plugins), no ProcessEvent hook. It is on when
//   * Shift is held while the game starts, or the command line has -SESafe, or LanNative\safe.txt exists, or
//   * the previous two starts never reached the first game tick (a crash-loop: LanNative\boot_state.txt counts starts that did not finish booting).
// The first tick resets the counter, so one clean safe-mode start brings the next start back to normal.
//
// Crash attribution: a vectored exception handler logs the first 40 access-violation-class exceptions with the faulting module (+ offset, so a plugin
// DLL is named) and a short stack of module+offset frames. Many are handled (the loader probes memory under __try); the LAST ones before a crash tell
// which code was running. The engine's own crash reporter still writes its dump to %LOCALAPPDATA%\Absolver\Saved\Crashes.
static wchar_t g_boot_path[MAX_PATH];
static char g_safe_why[160];                 // why safe mode is on (shown on the F1 Mods page)

static int safe_read_int(const wchar_t *path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); if (f == INVALID_HANDLE_VALUE) return 0;
    char b[32] = ""; DWORD r = 0; ReadFile(f, b, sizeof b - 1, &r, NULL); CloseHandle(f); b[r] = 0; return atoi(b);
}
static void safe_write_int(const wchar_t *path, int v) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (f == INVALID_HANDLE_VALUE) return;
    char b[32]; int n = snprintf(b, sizeof b, "%d\r\n", v); DWORD w; WriteFile(f, b, (DWORD)n, &w, NULL); CloseHandle(f);
}
static void safe_mode_detect(void) {
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ssafe.txt", g_dir); swprintf(g_boot_path, MAX_PATH, L"%sboot_state.txt", g_dir);
    const char *why = NULL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) why = "Shift was held at start";
    else if (cmdline_has_flag(GetCommandLineW(), L"sesafe")) why = "-SESafe on the command line";
    else if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) why = "LanNative\\safe.txt exists (delete it to leave safe mode)";
    int unfinished = safe_read_int(g_boot_path);
    if (!why && unfinished >= 2) why = "the last two starts never finished booting (crash-loop guard)";
    safe_write_int(g_boot_path, unfinished + 1);                          // assume this start dies until the first tick proves otherwise
    if (why) { g_safe_mode = 1; snprintf(g_safe_why, sizeof g_safe_why, "%s", why); logf_("SAFE MODE: %s. Mods, rows.json, plugins and the script extender hooks are OFF this run.", why); }
}
static void crashguard_ready(void) { if (g_boot_path[0]) safe_write_int(g_boot_path, 0); }

static volatile LONG g_veh_n;
static LONG CALLBACK se_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000094 && code != 0xC00000FD && code != 0xC0000096) return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&g_veh_n); if (n > 40) return EXCEPTION_CONTINUE_SEARCH;
    char line[900]; int w = 0; uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress; void *frames[12]; USHORT nf = RtlCaptureStackBackTrace(0, 12, frames, NULL);
    w += snprintf(line + w, sizeof line - (size_t)w, "exception #%ld 0x%08lX (first chance, may be handled) thread %lu at", (long)n, code, GetCurrentThreadId());
    for (int i = -1; i < (int)nf && w < (int)sizeof line - 120; i++) {
        uintptr_t a = i < 0 ? addr : (uintptr_t)frames[i]; HMODULE m = NULL; char name[60] = "?"; uintptr_t off = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
            wchar_t wn[MAX_PATH]; GetModuleFileNameW(m, wn, MAX_PATH); wchar_t *s = wcsrchr(wn, L'\\'); WideCharToMultiByte(CP_ACP, 0, s ? s + 1 : wn, -1, name, sizeof name, NULL, NULL); off = a - (uintptr_t)m;
        }
        w += snprintf(line + w, sizeof line - (size_t)w, " %s%s+0x%llX", i < 0 ? "" : "<", name, (unsigned long long)off);
        if (i < 0 && ep->ExceptionRecord->NumberParameters >= 2) w += snprintf(line + w, sizeof line - (size_t)w, " (access %llu of 0x%llX)", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0], (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    logf_("crash watch: %s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}
static void crashrep_install(void) { AddVectoredExceptionHandler(1, se_veh); }
