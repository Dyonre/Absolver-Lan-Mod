// overlay.cpp - the loader's Dear ImGui overlay, drawn on the render thread at IDXGISwapChain::Present.
// The Present / ResizeBuffers slots of the swap chain vtable are patched once, lazily (ov_request, first menu open); the vtable is found through a throwaway
// 8x8 device + swap chain, d3d11.dll is resolved at that moment (nothing is imported statically). The game window's WndProc is subclassed once the first
// frame is drawn. While the overlay is hidden nothing is drawn and no ImGui call is made: Present costs one counter increment and one flag test.
// Any exception inside a frame is caught; three of them switch the overlay off and the loader falls back to its text menu.
// Built without exceptions / RTTI; compiled into the same DLL as the C sources.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#include "overlay.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "bgkeys.h"
#include "bgact.h"
#include "gleamui.h"
#include <tlhelp32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *Resize_t)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI *CreateDevSc_t)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static int xi_install(void);                                       // the game's gamepad (below)
static volatile LONG g_xi_calls, g_xi_zeroed, g_xi_hooked;         // calls seen by the XInputGetState wrapper, calls it neutralised, 1 = installed / -1 = not possible
static void (*g_log)(const char *fmt, ...);
#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static Present_t o_present; static Resize_t o_resize;
static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx; static ID3D11RenderTargetView *g_rtv; static IDXGISwapChain *g_sc;
static HWND g_hwnd; static WNDPROC g_oldproc; static ImGuiContext *g_ictx;
static volatile LONG g_state = OV_OFF, g_vis, g_faults, g_present_calls, g_frames;
static volatile LONG g_dx, g_dy;                                   // raw mouse movement for the virtual cursor (a loader menu, or a plugin with OVM_MOUSE and no OVM_RAW)
static volatile LONG g_rdx, g_rdy;                                 // raw mouse movement for OVM_RAW (ov_raw_mouse)
static volatile LONG g_any;                                        // OR of every owner's mode flags
static volatile LONG g_want_keys;                                  // ImGui wants the keyboard this frame (io.WantCaptureKeyboard / WantTextInput)
static volatile LONG g_focus = 1, g_clear;                         // the game window has the focus; a focus loss asks the next frame to clear ImGui's input
static unsigned g_mode[64];                                       // per owner (plugin index)
static unsigned char g_swallowed[256];                            // keys whose press was kept from the game: their release is kept too (window thread only)
typedef struct { int owner; void (*draw)(void *); void *user; } DrawReg;
#define DRAWMAX 16
static DrawReg g_reg[DRAWMAX]; static int g_nreg;
static int (*g_alive)(int); static void (*g_fault)(int, unsigned long); static const char *(*g_name)(int);
static int g_cw, g_ch; static float g_scale = 1.f;
static ULONGLONG g_req_tick;
static float g_vx = -1, g_vy = -1, g_min_scale = 1.f;
static int g_imgui, g_was_vis;
static volatile LONG g_installed;                                  // set once the hooks are in (or the attempt failed); everything but ov_request waits for it
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_cs;                                      // guards the tables, g_pub, the command queue and the ImGui context (the frame and the WndProc hold it while they use ImGui)
static OvSnap g_pub, g_cur; static unsigned g_pub_seq, g_cur_seq;
#define CMDQ 8
static OvCmd g_q[CMDQ]; static int g_qn;
// input focus and the cursor
static volatile unsigned char g_keep[256];                         // F keys a mod uses: kept from the game's window procedure (ov_keep_key)
static volatile LONG g_chord_guard;                                // the game does not see Start while Back is held (Back + Start = the loader menu)
static volatile LONG g_focus_owner = -2;                           // ov_focus_owner: -2 nobody, -1 a loader window, >= 0 a plugin window that takes keys
static volatile LONG g_cursor_mode;                                // 0 none, 1 virtual cursor, 2 the game's OS cursor
static ImGuiStorage g_win_owner, g_win_frame;                      // window id -> owner + 2, frame it was last seen (render thread)
static volatile LONG g_tabkey_q, g_tabkey_e, g_tabkey_esc;         // Q / E / Esc presses while a loader menu shows (window thread counts, draw_mods takes them)
static volatile LONG g_text_input;                                 // 1 = an overlay text box has the keyboard (set each frame)
static char g_notice[400];                                         // the host's notices (ov_set_notice; guarded by g_cs)
static volatile LONG g_bgm_x = -1, g_bgm_y = -1;                   // -LNBgInput: the last posted mouse position (client px); -1 = none yet

extern "C" void ov_set_log(void (*fn)(const char *fmt, ...)) { g_log = fn; }
extern "C" void ov_set_guard(int (*alive)(int), void (*fault)(int, unsigned long), const char *(*name)(int)) { g_alive = alive; g_fault = fault; g_name = name; }
static const char *owner_name(int o) { const char *n = g_name ? g_name(o) : NULL; return n ? n : "?"; }
static void recompute_any(void) {                                    // caller holds g_cs
    unsigned a = 0; for (int i = 0; i < 64; i++) a |= g_mode[i];
    InterlockedExchange(&g_any, (LONG)a);
}
extern "C" int ov_register(int owner, void (*draw)(void *), void *user) {
    if (!draw || owner < 0 || owner > 63 || !g_installed) return 1;
    EnterCriticalSection(&g_cs); int rc = 1;
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].owner == owner && g_reg[i].draw == draw && g_reg[i].user == user) { LeaveCriticalSection(&g_cs); return 1; }
    if (g_nreg < DRAWMAX) { g_reg[g_nreg].owner = owner; g_reg[g_nreg].draw = draw; g_reg[g_nreg].user = user; g_nreg++; rc = 0; }
    LeaveCriticalSection(&g_cs);
    return rc;
}
extern "C" int ov_unregister(int owner, void (*draw)(void *), void *user) {
    if (!g_installed) return 1;
    EnterCriticalSection(&g_cs); int rc = 1;
    for (int i = 0; i < g_nreg; i++) if (g_reg[i].owner == owner && g_reg[i].draw == draw && g_reg[i].user == user) { memmove(g_reg + i, g_reg + i + 1, sizeof(DrawReg) * (size_t)(g_nreg - i - 1)); g_nreg--; rc = 0; break; }
    LeaveCriticalSection(&g_cs);
    return rc;
}
extern "C" int ov_set_mode(int owner, unsigned flags) {
    if (owner < 0 || owner > 63 || !g_installed) return 1;
    flags &= OVM_DRAW | OVM_MOUSE | OVM_KEYS | OVM_RAW | OVM_BLOCK | OVM_HIDECURSOR;
    if (flags & OVM_BLOCK) xi_install();
    EnterCriticalSection(&g_cs);
    unsigned old = g_mode[owner], was_mouse = (unsigned)(g_any & OVM_MOUSE); g_mode[owner] = flags; recompute_any();
    if (flags != old) { InterlockedExchange(&g_rdx, 0); InterlockedExchange(&g_rdy, 0); }
    if (was_mouse && !(g_any & OVM_MOUSE)) InterlockedExchange(&g_clear, 1);   // the mouse is no longer ImGui's: a button it still holds is let go
    LeaveCriticalSection(&g_cs);
    if (flags != old) LOG("overlay: plugin %s mode 0x%x -> 0x%x%s", owner_name(owner), old, flags, g_xi_hooked > 0 ? " (gamepad hook: calls and neutralised so far are logged below)" : "");
    if (flags != old && g_xi_hooked > 0) LOG("overlay: XInputGetState seen %ld time(s), %ld neutralised", g_xi_calls, g_xi_zeroed);
    return 0;
}
static int tab_drop_owner(int owner);                              // mod tabs (below)
extern "C" void ov_release_owner(int owner) {
    if (owner < 0 || owner > 63 || !g_installed) return;
    EnterCriticalSection(&g_cs);
    unsigned old = g_mode[owner], was_mouse = (unsigned)(g_any & OVM_MOUSE); int dropped = 0;
    g_mode[owner] = 0; recompute_any();
    if (was_mouse && !(g_any & OVM_MOUSE)) InterlockedExchange(&g_clear, 1);
    for (int i = 0; i < g_nreg; ) { if (g_reg[i].owner == owner) { memmove(g_reg + i, g_reg + i + 1, sizeof(DrawReg) * (size_t)(g_nreg - i - 1)); g_nreg--; dropped++; } else i++; }
    dropped += tab_drop_owner(owner);
    LeaveCriticalSection(&g_cs);
    if (old || dropped) LOG("overlay: plugin %s released (mode 0x%x, %d callback(s) removed)", owner_name(owner), old, dropped);
}
extern "C" int ov_raw_mouse(int *dx, int *dy) {
    if (!g_installed) return 1;
    *dx = (int)InterlockedExchange(&g_rdx, 0); *dy = (int)InterlockedExchange(&g_rdy, 0);
    return 0;
}
extern "C" void ov_info(int owner, OvInfo *o) {
    memset(o, 0, sizeof *o);
    LONG st = g_state; o->state = st == OV_READY ? 1 : st == OV_DEAD ? -1 : 0;
    o->context = g_ictx; o->version = IMGUI_VERSION; o->version_num = IMGUI_VERSION_NUM;
    if (g_ictx) { ImGuiMemAllocFunc a; ImGuiMemFreeFunc f; void *u; ImGui::GetAllocatorFunctions(&a, &f, &u); o->alloc = a; o->free = f; o->alloc_user = u; }
    o->width = g_cw; o->height = g_ch; o->scale = g_scale; o->device = g_dev; o->device_context = g_ctx;
    o->sizeof_io = (int)sizeof(ImGuiIO); o->sizeof_style = (int)sizeof(ImGuiStyle); o->sizeof_vert = (int)sizeof(ImDrawVert); o->sizeof_idx = (int)sizeof(ImDrawIdx); o->sizeof_wchar = (int)sizeof(ImWchar);
    if (owner >= 0 && owner < 64 && g_installed) {
        EnterCriticalSection(&g_cs); o->caller_mode = g_mode[owner]; for (int i = 0; i < g_nreg; i++) if (g_reg[i].owner == owner) o->caller_callbacks++; LeaveCriticalSection(&g_cs);
    }
}
extern "C" void ov_set_scale(float s) { g_min_scale = s < 0.5f ? 0.5f : s > 4.f ? 4.f : s; }
extern "C" int ov_state(void) { return (int)g_state; }
extern "C" int ov_shows_menu(void) {
    LONG st = g_state;
    return st == OV_READY || (st == OV_STARTING && GetTickCount64() - g_req_tick < 2500);
}
extern "C" int ov_poll(OvCmd *c) {
    int got = 0; if (!g_installed) return 0;
    EnterCriticalSection(&g_cs);
    if (g_qn > 0) { *c = g_q[0]; memmove(g_q, g_q + 1, sizeof(OvCmd) * (size_t)(g_qn - 1)); g_qn--; got = 1; }
    LeaveCriticalSection(&g_cs);
    return got;
}
static void post(int cmd, int index, int arg = 0, const char *text = NULL) {   // render thread; the caller holds g_cs (it is inside the frame)
    if (g_qn < CMDQ) { OvCmd &c = g_q[g_qn]; c.cmd = cmd; c.index = index; c.arg = arg; snprintf(c.text, sizeof c.text, "%s", text ? text : ""); g_qn++; }
}
extern "C" void ov_publish(const OvSnap *s) {
    if (!g_installed) return;
    int shown = 0;
    EnterCriticalSection(&g_cs);
    if (!s || s->kind == OV_NONE) { g_pub.kind = OV_NONE; InterlockedExchange(&g_vis, 0); }
    else {
        if (memcmp(&g_pub, s, sizeof g_pub) != 0) { memcpy(&g_pub, s, sizeof g_pub); g_pub_seq++; }
        InterlockedExchange(&g_vis, g_state == OV_DEAD ? 0 : 1); shown = g_state != OV_DEAD;
    }
    LeaveCriticalSection(&g_cs);
    if (shown) xi_install();                                         // a loader menu on screen: the game gets a neutral gamepad (the menu reads the pad itself)
}
extern "C" int ov_focus_owner(void) { return (int)g_focus_owner; }
extern "C" void ov_set_notice(const char *text) { if (!g_installed) return; EnterCriticalSection(&g_cs); snprintf(g_notice, sizeof g_notice, "%s", text ? text : ""); LeaveCriticalSection(&g_cs); }
extern "C" int ov_cursor_mode(void) { return (int)g_cursor_mode; }
extern "C" void ov_keep_key(int vk, int on) {
    if (vk <= 0 || vk > 255) return;
    if (!!g_keep[vk] != !!on) LOG("overlay: key 0x%02X %s the game's window procedure", vk, on ? "is now kept from" : "reaches again");
    g_keep[vk] = (unsigned char)(on != 0);
}
extern "C" void ov_pad_chord_guard(int on) {
    if (on) xi_install();
    if (InterlockedExchange(&g_chord_guard, on ? 1 : 0) != (on ? 1 : 0)) LOG("overlay: gamepad chord guard %s (the game %s Start while Back is held)", on ? "on" : "off", on ? "does not see" : "sees");
}

/* ---------- background mode : -LNBgInput on the command line, never on a player's ---------- */
// A test driver posts keys to an unfocused game window and the game must never take the foreground from the user's windows:
//  1. the loader's binds / on_key / test keys read the window's own key messages (bgkeys.h table) instead of the real keyboard;
//  2. activation guard: UE4 activates its own window after creating it, so the game's (and every mod DLL's) user32 imports SetForegroundWindow / SetActiveWindow /
//     SetFocus / BringWindowToTop / SwitchToThisWindow / AttachThreadInput become no-ops and ShowWindow / ShowWindowAsync / SetWindowPos lose their activation
//     (bgact.h decides); WM_MOUSEACTIVATE answers MA_NOACTIVATE;
//  3. mods (DLLs under a Mods folder) also see the game window as the foreground window and the window's own key state in GetAsyncKeyState, so their own
//     focus checks and polls work on an unfocused window with no change to the mod.
static int bg_on(void) { static int v = -1; if (v < 0) v = bgk_flag(GetCommandLineW()); return v; }
static HWND g_bg_hwnd; static WNDPROC g_bg_old;
static volatile LONG g_bg_msgn, g_bg_seen[256];
static void bg_key_msg(UINT m, WPARAM w) {                             // window thread: every key message of the game window (why was a posted key not acked? this log answers)
    bgk_msg(m, w, GetTickCount64());
    LONG n = InterlockedIncrement(&g_bg_msgn);
    if (n <= 40) LOG("overlay: -LNBgInput: key message %s vk 0x%02X (#%ld)", (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? "down" : "up", (unsigned)(w & 0xFF), (long)n);
}
extern "C" int ov_bg_input_on(void) { return bg_on(); }
extern "C" int ov_bg_key_down(int vk) {
    int d = bgk_down(vk, GetTickCount64());
    if (d && vk > 0 && vk < 256 && !InterlockedExchange(&g_bg_seen[vk], 1)) LOG("overlay: -LNBgInput: vk 0x%02X first read as down by a bind / mod (so the posted key reached the loader's table)", vk);
    return d;
}
// ---- the activation guard
static BOOL (WINAPI *o_ShowWindow)(HWND, int); static BOOL (WINAPI *o_ShowWindowAsync)(HWND, int); static BOOL (WINAPI *o_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static SHORT (WINAPI *o_GetAsyncKeyState)(int); static HWND (WINAPI *o_GetForegroundWindow)(void);
static volatile LONG g_bga_n[16];
static void bga_note(int i, const char *fn) {                          // the first few refusals of each call are logged, then every 1000th
    LONG n = InterlockedIncrement(&g_bga_n[i]);
    if (n <= 3 || n % 1000 == 0) LOG("overlay: -LNBgInput: activation refused: %s (call %ld)", fn, (long)n);
}
static BOOL WINAPI bga_SetForegroundWindow(HWND h) { (void)h; bga_note(0, "SetForegroundWindow"); return TRUE; }
static HWND WINAPI bga_SetActiveWindow(HWND h) { (void)h; bga_note(1, "SetActiveWindow"); return GetActiveWindow(); }
static HWND WINAPI bga_SetFocus(HWND h) { (void)h; bga_note(2, "SetFocus"); return GetFocus(); }
static BOOL WINAPI bga_BringWindowToTop(HWND h) { (void)h; bga_note(3, "BringWindowToTop"); return TRUE; }
static void WINAPI bga_SwitchToThisWindow(HWND h, BOOL b) { (void)h; (void)b; bga_note(4, "SwitchToThisWindow"); }
static BOOL WINAPI bga_AttachThreadInput(DWORD a, DWORD b, BOOL f) { (void)a; (void)b; (void)f; bga_note(5, "AttachThreadInput"); return TRUE; }   // it would share the input state with the user's app
static BOOL WINAPI bga_ShowWindow(HWND h, int c) { int n = bga_showcmd(c); if (n != c) bga_note(6, "ShowWindow (made non-activating)"); return o_ShowWindow ? o_ShowWindow(h, n) : FALSE; }
static BOOL WINAPI bga_ShowWindowAsync(HWND h, int c) { int n = bga_showcmd(c); if (n != c) bga_note(7, "ShowWindowAsync (made non-activating)"); return o_ShowWindowAsync ? o_ShowWindowAsync(h, n) : FALSE; }
static BOOL WINAPI bga_SetWindowPos(HWND h, HWND a, int x, int y, int cx, int cy, UINT f) {
    if (!(f & SWP_NOACTIVATE)) bga_note(8, "SetWindowPos (made non-activating)");
    return o_SetWindowPos ? o_SetWindowPos(h, a, x, y, cx, cy, bga_swpflags(f)) : FALSE;
}
static HWND WINAPI bga_GetForegroundWindow(void) { return g_bg_hwnd ? g_bg_hwnd : (o_GetForegroundWindow ? o_GetForegroundWindow() : NULL); }   // mods: "the game window has the focus"
static SHORT WINAPI bga_GetAsyncKeyState(int vk) { return bga_asynckey(vk, bgk_down(vk, GetTickCount64())); }                                          // mods: the window's own key state
typedef struct { const char *name; void *hook; } BgImp;
static const BgImp g_bga_act[] = { { "SetForegroundWindow", (void *)bga_SetForegroundWindow }, { "SetActiveWindow", (void *)bga_SetActiveWindow }, { "SetFocus", (void *)bga_SetFocus },
    { "BringWindowToTop", (void *)bga_BringWindowToTop }, { "SwitchToThisWindow", (void *)bga_SwitchToThisWindow }, { "AttachThreadInput", (void *)bga_AttachThreadInput },
    { "ShowWindow", (void *)bga_ShowWindow }, { "ShowWindowAsync", (void *)bga_ShowWindowAsync }, { "SetWindowPos", (void *)bga_SetWindowPos } };
static const BgImp g_bga_mod[] = { { "GetForegroundWindow", (void *)bga_GetForegroundWindow }, { "GetAsyncKeyState", (void *)bga_GetAsyncKeyState } };
// route the named USER32 imports of one module through the hooks (import address table, like xi_install); returns how many slots were changed
static int bga_patch_one(HMODULE mod, const BgImp *list, int n) {
    int hit = 0; BYTE *base = (BYTE *)mod; if (!base) return 0;
    __try {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dd.VirtualAddress) return 0;
        for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
            if (_stricmp((const char *)(base + d->Name), "user32.dll") != 0) continue;
            ULONGLONG *thunk = (ULONGLONG *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)); void **iat = (void **)(base + d->FirstThunk);
            for (int i = 0; thunk[i]; i++) {
                if (thunk[i] & IMAGE_ORDINAL_FLAG64) continue;
                const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + thunk[i]))->Name;
                for (int k = 0; k < n; k++) {
                    if (strcmp(fn, list[k].name) != 0 || iat[i] == list[k].hook) continue;
                    DWORD old; if (!VirtualProtect(&iat[i], sizeof(void *), PAGE_READWRITE, &old)) continue;
                    iat[i] = list[k].hook; VirtualProtect(&iat[i], sizeof(void *), old, &old); hit++;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("overlay: -LNBgInput: EXCEPTION 0x%08lX while reading a module's imports - skipped", GetExceptionCode()); }
    return hit;
}
static int g_bga_inst, g_bga_routed = -1;
extern "C" int ov_bg_summary(char *out, int cap) {                    // total refused / changed activation calls so far, and which (for the periodic log line; 0 = none)
    static const char *const nm[9] = { "SetForegroundWindow", "SetActiveWindow", "SetFocus", "BringWindowToTop", "SwitchToThisWindow", "AttachThreadInput", "ShowWindow", "ShowWindowAsync", "SetWindowPos" };
    int total = 0, len = 0; if (out && cap > 0) out[0] = 0;
    for (int i = 0; i < 9; i++) { LONG n = g_bga_n[i]; if (!n) continue; total += (int)n; if (out && len < cap - 40) len += snprintf(out + len, (size_t)(cap - len), "%s%s x%ld", len ? ", " : "", nm[i], (long)n); }
    return total;
}
extern "C" void ov_bg_guard_install(void) {                          // as early as possible (before the game creates its window): the game's own imports
    if (!bg_on() || g_bga_inst) return; g_bga_inst = 1;
    HMODULE u = GetModuleHandleW(L"user32.dll"); if (!u) { LOG("overlay: -LNBgInput: user32.dll not loaded - activation guard NOT installed"); return; }
    o_ShowWindow = (BOOL (WINAPI *)(HWND, int))(void *)GetProcAddress(u, "ShowWindow"); o_ShowWindowAsync = (BOOL (WINAPI *)(HWND, int))(void *)GetProcAddress(u, "ShowWindowAsync");
    o_SetWindowPos = (BOOL (WINAPI *)(HWND, HWND, int, int, int, int, UINT))(void *)GetProcAddress(u, "SetWindowPos");
    o_GetAsyncKeyState = (SHORT (WINAPI *)(int))(void *)GetProcAddress(u, "GetAsyncKeyState"); o_GetForegroundWindow = (HWND (WINAPI *)(void))(void *)GetProcAddress(u, "GetForegroundWindow");
    int n = bga_patch_one(GetModuleHandleW(NULL), g_bga_act, (int)(sizeof g_bga_act / sizeof g_bga_act[0])); g_bga_routed = n;   // (no log sink yet at start-up: bg_hook logs this once the overlay has one)
    LOG("overlay: -LNBgInput: activation guard installed in the game's import table (%d of %d activating USER32 imports routed; 0 means this build imports none of them)", n, (int)(sizeof g_bga_act / sizeof g_bga_act[0]));
}
extern "C" void ov_bg_mods_patch(void) {                             // every few seconds: mod DLLs (a hot reload loads new ones) get the guard + the focus / key reads
    if (!bg_on() || !g_bga_inst) return;
    // no "done" list: a hot-reloaded mod can load at the same base with a fresh import table; bga_patch_one skips slots already hooked, so a module counts only when it changed
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId()); if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me; me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        char p[MAX_PATH]; if (!WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, p, sizeof p, NULL, NULL) || !bga_is_mod_path(p)) continue;
        int a = bga_patch_one(me.hModule, g_bga_act, (int)(sizeof g_bga_act / sizeof g_bga_act[0])), b = bga_patch_one(me.hModule, g_bga_mod, (int)(sizeof g_bga_mod / sizeof g_bga_mod[0]));
        if (a || b) LOG("overlay: -LNBgInput: mod %ls: %d activating import(s) guarded, %d focus / key import(s) routed to the window's own state", me.szModule, a, b);
    }
    CloseHandle(snap);
}
static LRESULT CALLBACK bg_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    long long r; if (bga_wndmsg(m, &r)) return (LRESULT)r;            // WM_MOUSEACTIVATE: a click never activates
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP) bg_key_msg(m, w);
    return CallWindowProcW(g_bg_old, h, m, w, l);
}
static void bg_hook(IDXGISwapChain *sc) {                              // render thread, first Present: subclass the window once (g_imgui_init later chains on top of this)
    static int tried; if (tried) return; tried = 1;
    DXGI_SWAP_CHAIN_DESC d; if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) { LOG("overlay: -LNBgInput: no output window - background key input is NOT available"); return; }
    { char sm[256]; int t = ov_bg_summary(sm, sizeof sm); LOG("overlay: -LNBgInput: activation guard in the game's import table: %d of %d activating USER32 imports routed (made at start-up); %d call(s) refused / made non-activating so far%s%s", g_bga_routed, (int)(sizeof g_bga_act / sizeof g_bga_act[0]), t, t ? ": " : "", sm); }
    g_bg_hwnd = d.OutputWindow; g_bg_old = (WNDPROC)SetWindowLongPtrW(g_bg_hwnd, GWLP_WNDPROC, (LONG_PTR)bg_wndproc);
    LOG("overlay: -LNBgInput: game window %p subclassed for background key input (old %p); binds read the window's own key messages, not the real keyboard", (void *)g_bg_hwnd, (void *)g_bg_old);
}
/* ---------- input: while the overlay takes input, the game window's messages go to ImGui first ---------- */
// where a raw mouse movement goes: ov_raw_mouse while a plugin has OVM_RAW, the virtual cursor for a loader menu or a plugin with OVM_MOUSE (not while OVM_RAW is set, unless a menu is open)
static void raw_move(LONG dx, LONG dy, LONG menu, LONG mouse_on, LONG raw_on) {
    if (raw_on) { InterlockedAdd(&g_rdx, dx); InterlockedAdd(&g_rdy, dy); }
    if (mouse_on && (menu || !raw_on)) { InterlockedAdd(&g_dx, dx); InterlockedAdd(&g_dy, dy); }
}
static int is_fkey(UINT m, WPARAM w) { return (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w >= VK_F1 && w <= VK_F12; }
// mouse button of a button message: bit 0 left, 1 right, 2 middle, 3 / 4 the X buttons; down = 1 press, 0 release, -1 not a button message
static int mouse_button(UINT m, WPARAM w, int *down) {
    *down = -1;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: *down = 1; return 1;
    case WM_LBUTTONUP: *down = 0; return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: *down = 1; return 2;
    case WM_RBUTTONUP: *down = 0; return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: *down = 1; return 4;
    case WM_MBUTTONUP: *down = 0; return 4;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: *down = 1; return GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 8 : 16;
    case WM_XBUTTONUP: *down = 0; return GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 8 : 16;
    }
    return 0;
}
enum { R_PASS = 0, R_SWALLOW, R_DEFAULT };                         // what the window procedure does with a message: give it to the game, drop it, or only let the default procedure release it (WM_INPUT)
static unsigned char g_game_btn, g_kept_btn;                       // mouse buttons whose press the game saw / whose press was kept from it (window thread only)
// Decides for one message. feed(...) hands it to ImGui. Window thread only.
static int route(HWND h, UINT m, WPARAM w, LPARAM l, void (*feed)(HWND, UINT, WPARAM, LPARAM)) {
    if (bg_on() && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP)) bg_key_msg(m, w);   // -LNBgInput: the key table of the game window (bgkeys.h)
    if (bg_on()) InterlockedExchange(&g_focus, 1);                                   // -LNBgInput: the window never has the real focus; its posted messages are the input
    if (bg_on() && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) { InterlockedExchange(&g_bgm_x, (LONG)(short)LOWORD(l)); InterlockedExchange(&g_bgm_y, (LONG)(short)HIWORD(l)); }   // posted mouse = the cursor
    else if (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) {   // focus lost: nothing is swallowed until it returns, ImGui forgets held keys and buttons
        InterlockedExchange(&g_focus, 0); InterlockedExchange(&g_clear, 1); InterlockedExchange(&g_rdx, 0); InterlockedExchange(&g_rdy, 0);
    } else if (m == WM_SETFOCUS || (m == WM_ACTIVATEAPP && w) || (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE)) InterlockedExchange(&g_focus, 1);
    if ((m == WM_KEYUP || m == WM_SYSKEYUP) && w < 256 && g_swallowed[w]) {         // the game never saw this key go down: it must not see it come up
        g_swallowed[w] = 0; if (feed) feed(h, m, w, l); return R_SWALLOW;
    }
    int down, btn = mouse_button(m, w, &down);
    if (down == 0 && (g_kept_btn & btn)) { g_kept_btn &= (unsigned char)~btn; if (feed) feed(h, m, w, l); return R_SWALLOW; }   // same for a mouse button
    if (g_focus && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w < 256 && g_keep[w]) { g_swallowed[w] = 1; return R_SWALLOW; }   // an F key a mod owns (F11 = the game's fullscreen toggle too): the game never sees it
    LONG menu = g_vis, any = g_any;
    if (menu && g_focus && (m == WM_KEYDOWN) && !(l & 0x40000000) && !g_text_input) {   // first press only (not auto-repeat), not while typing
        if (w == 'Q') InterlockedIncrement(&g_tabkey_q); else if (w == 'E') InterlockedIncrement(&g_tabkey_e); else if (w == VK_ESCAPE) InterlockedIncrement(&g_tabkey_esc);
    }
    LONG block = any & OVM_BLOCK;                                    // keep every key and mouse message from the game
    LONG mouse_on = menu || (any & OVM_MOUSE), keys_on = menu || (any & OVM_KEYS), raw_on = any & OVM_RAW;
    if (g_focus && g_state == OV_READY && (mouse_on || keys_on || raw_on || block)) {
        if (m == WM_INPUT && (mouse_on || raw_on || block)) {        // the game reads raw mouse movement for the camera: it goes to the virtual cursor or to ov_raw_mouse instead
            RAWINPUT ri; UINT sz = sizeof ri;
            if (l && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                raw_move(ri.data.mouse.lLastX, ri.data.mouse.lLastY, menu, mouse_on, raw_on);
            return R_DEFAULT;
        }
        int is_mouse = m >= WM_MOUSEFIRST && m <= WM_MOUSELAST;
        if ((mouse_on || block) && (m == WM_MOUSEMOVE || m == WM_NCMOUSEMOVE || m == WM_MOUSELEAVE || m == WM_NCMOUSELEAVE)) return R_SWALLOW;   // the game pins the OS cursor; only the virtual cursor counts
        int is_key = m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR || m == WM_SYSCHAR;
        if (((is_mouse && mouse_on) || (is_key && keys_on)) && feed) feed(h, m, w, l);
        if (is_mouse && (mouse_on || block)) {
            if (down == 0 && (g_game_btn & btn)) { g_game_btn &= (unsigned char)~btn; return R_PASS; }   // the game saw this press: it gets the release, so nothing stays held
            if (down == 1) g_kept_btn |= (unsigned char)btn;
            return R_SWALLOW;
        }
        int key_down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_SYSCHAR;
        if (key_down && !is_fkey(m, w) && ((keys_on && (menu || g_want_keys)) || block)) { if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w < 256) g_swallowed[w] = 1; return R_SWALLOW; }   // the F keys keep working; a swallowed press takes its release with it
    }
    if (down == 1) g_game_btn |= (unsigned char)btn; else if (down == 0) g_game_btn &= (unsigned char)~btn;
    return R_PASS;
}
static void feed_imgui(HWND h, UINT m, WPARAM w, LPARAM l) { if (!g_imgui) return; EnterCriticalSection(&g_cs); ImGui_ImplWin32_WndProcHandler(h, m, w, l); LeaveCriticalSection(&g_cs); }
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    long long bgr; if (bg_on() && bga_wndmsg(m, &bgr)) return (LRESULT)bgr;   // -LNBgInput: WM_MOUSEACTIVATE -> MA_NOACTIVATE
    int r = route(h, m, w, l, feed_imgui);
    if (r == R_SWALLOW) return 0;
    if (r == R_DEFAULT) return DefWindowProcW(h, m, w, l);           // the default procedure releases the raw input buffer
    return CallWindowProcW(g_oldproc, h, m, w, l);
}

/* ---------- the game's gamepad: while a plugin has OVM_BLOCK the game's XInputGetState reports a neutral pad ---------- */
typedef DWORD(WINAPI *XInputGetState_t)(DWORD, void *);
static XInputGetState_t g_xi_orig;
static void xi_neutral(void *state) { memset((char *)state + sizeof(DWORD), 0, 12); }   // XINPUT_STATE: the packet number stays, buttons, triggers and thumbs (12 bytes) go to zero
static DWORD WINAPI xi_get_state(DWORD user, void *state) {
    DWORD rc = g_xi_orig ? g_xi_orig(user, state) : ERROR_DEVICE_NOT_CONNECTED;
    InterlockedIncrement(&g_xi_calls);
    if (rc == ERROR_SUCCESS && state) {
        WORD *b = (WORD *)((char *)state + sizeof(DWORD));             // XINPUT_GAMEPAD.wButtons
        if (g_chord_guard && (*b & 0x0020)) *b &= (WORD)~0x0010;       // Back held: Start is the loader menu chord, not the game's pause menu
        if (((g_any & OVM_BLOCK) || g_vis) && g_focus) { xi_neutral(state); InterlockedIncrement(&g_xi_zeroed); }   // a loader menu or a BLOCK_GAME window has the pad
    }
    return rc;
}
// The game imports XInputGetState by ordinal (xinput1_3 ordinal 2): replace its slot in the executable's import address table. Once; 1 = in place.
static int xi_install(void) {
    if (g_xi_hooked) return g_xi_hooked > 0;
    g_xi_hooked = -1;
    BYTE *base = (BYTE *)GetModuleHandleW(NULL); if (!base) return 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        if (_strnicmp((const char *)(base + d->Name), "xinput", 6) != 0) continue;
        ULONGLONG *thunk = (ULONGLONG *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)); void **iat = (void **)(base + d->FirstThunk);
        for (int i = 0; thunk[i]; i++) {
            int is_get_state = 0;
            if (thunk[i] & IMAGE_ORDINAL_FLAG64) is_get_state = (thunk[i] & 0xFFFF) == 2;
            else is_get_state = strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + thunk[i]))->Name, "XInputGetState") == 0;
            if (!is_get_state) continue;
            DWORD old; if (!VirtualProtect(&iat[i], sizeof(void *), PAGE_READWRITE, &old)) return 0;
            g_xi_orig = (XInputGetState_t)iat[i]; iat[i] = (void *)xi_get_state; VirtualProtect(&iat[i], sizeof(void *), old, &old);
            g_xi_hooked = 1; LOG("overlay: the game's XInputGetState is routed through the overlay (import slot %p)", (void *)&iat[i]); return 1;
        }
    }
    LOG("overlay: no XInputGetState import found - ABS_OV_BLOCK_GAME cannot neutralise the gamepad");
    return 0;
}

/* ---------- the game's cursor position (no game hover under the loader's windows): while the game shows its own cursor
 * (cursor mode 2: pause / Esc menu, inventory) and the mouse is over an overlay window, the game's GetCursorPos reports a point far outside its window,
 * so the game's menu underneath shows no hover (Slate re-reads the cursor every frame; swallowing mouse messages alone does not stop the hover).
 * Only the executable's import is routed; the overlay's own calls (ImGui) go through this DLL's import and see the real position. ---------- */
typedef BOOL(WINAPI *GetCursorPos_t)(LPPOINT);
static GetCursorPos_t g_gcp_orig; static volatile LONG g_gcp_hooked, g_mouse_on_ui, g_gcp_hidden;
static BOOL WINAPI gcp_hook(LPPOINT p) {
    BOOL ok = g_gcp_orig ? g_gcp_orig(p) : FALSE;
    if (ok && p && g_mouse_on_ui && g_cursor_mode == 2 && g_state == OV_READY) {
        p->x = -32000; p->y = -32000;
        if (InterlockedIncrement(&g_gcp_hidden) == 1) LOG("overlay: the mouse is over an overlay window - the game's cursor position is hidden from it meanwhile (no hover in the game's menu underneath)");
    }
    return ok;
}
static int gcp_install(void) {                                     // once; 1 = in place
    if (g_gcp_hooked) return g_gcp_hooked > 0;
    g_gcp_hooked = -1;
    BYTE *base = (BYTE *)GetModuleHandleW(NULL); if (!base) return 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), "user32.dll") != 0) continue;
        ULONGLONG *thunk = (ULONGLONG *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)); void **iat = (void **)(base + d->FirstThunk);
        for (int i = 0; thunk[i]; i++) {
            if ((thunk[i] & IMAGE_ORDINAL_FLAG64) || strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(base + thunk[i]))->Name, "GetCursorPos") != 0) continue;
            DWORD old; if (!VirtualProtect(&iat[i], sizeof(void *), PAGE_READWRITE, &old)) return 0;
            g_gcp_orig = (GetCursorPos_t)iat[i]; iat[i] = (void *)gcp_hook; VirtualProtect(&iat[i], sizeof(void *), old, &old);
            g_gcp_hooked = 1; LOG("overlay: the game's GetCursorPos is routed through the overlay (import slot %p): no game hover under overlay windows", (void *)&iat[i]); return 1;
        }
    }
    LOG("overlay: no GetCursorPos import in the game - its menus may still highlight under an overlay window");
    return 0;
}

/* ---------- drawing ---------- */
static void tt(const char *s) { if (ImGui::IsItemHovered() && s && s[0]) ImGui::SetTooltip("%s", s); }

static void draw_lines(const OvSnap &s) {
    for (int i = 0; i < s.count; i++) {
        if (i == 0) ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f), "%s", s.label[i]); else ImGui::TextUnformatted(s.label[i]);
    }
}
// One highlight for mouse and keyboard: moving the mouse onto a row makes it the selected row (OVC_MENU_HOVER), so the next Enter / A acts on what is
// under the cursor. A selection moved by the keys or the pad scrolls into view; one that came from the mouse does not (the list would move under it).
static int g_hover_posted = -1;
static void row_hover(int i, int sel) {
    if (ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0.f || ImGui::GetIO().MouseDelta.y != 0.f) && i != sel && g_hover_posted != i) { g_hover_posted = i; post(OVC_MENU_HOVER, i); }
}
static void draw_menu(const OvSnap &s) {
    static int last_sel = -1; int scroll_to = s.sel != last_sel && s.sel != g_hover_posted; if (s.sel != last_sel && s.sel != g_hover_posted) g_hover_posted = -1; last_sel = s.sel;
    ImGui::TextDisabled("%s", s.title);
    if (s.searchable) {
        if (s.filter[0]) ImGui::TextColored(ImVec4(1.f, 0.85f, 0.35f, 1.f), "Search: %s_", s.filter);
        else ImGui::TextDisabled("Type to search");
    }
    float foot = ImGui::GetFrameHeightWithSpacing() * 2.f + ImGui::GetTextLineHeightWithSpacing() + 4.f;
    ImGui::BeginChild("rows", ImVec2(0, -foot), ImGuiChildFlags_Borders);
    if (!s.count && s.empty[0]) ImGui::TextDisabled("%s", s.empty);
    for (int i = 0; i < s.count; i++) {
        if (i == s.sel) ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));   // the selected row stays strong under a resting mouse
        bool hit = ImGui::Selectable(s.label[i], i == s.sel);
        if (i == s.sel) ImGui::PopStyleColor();
        if (hit) post(OVC_MENU_SELECT, i);
        row_hover(i, s.sel);
        if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.5f);
    }
    ImGui::EndChild();
    if (s.has_back) { if (ImGui::Button("Back")) post(OVC_MENU_BACK, 0); ImGui::SameLine(); }
    if (ImGui::Button("Close")) post(OVC_TOGGLE, 0);
    ImGui::TextDisabled("Up/Down + Enter (or click)   Esc / Left = back   F2 closes   Pad: d-pad, A, B");
    ImGui::TextDisabled("F1: the loader window (mods, keybinds, hosting, log and the mods' own tabs)");
}

static void content_text(const OvMod &m, char *out, size_t cap) {
    int w = 0; out[0] = 0;
    if (m.paks_total) w += snprintf(out + w, cap - (size_t)w, "%d/%d pak  ", m.paks_ok, m.paks_total);
    if (m.rows_ops) w += snprintf(out + w, cap - (size_t)w, "%d row edit(s)  ", m.rows_ops);
    if (m.content_files) w += snprintf(out + w, cap - (size_t)w, "%d content  ", m.content_files);
    if (m.plugin == 1) w += snprintf(out + w, cap - (size_t)w, "plugin  ");
    if (!m.affects_gameplay) snprintf(out + w, cap - (size_t)w, "[cosmetic]");
    size_t l = strlen(out);                                                         // net / joiners_need tags (bounded: the snprintf chain above may have run past cap)
    if (m.net_tag[0] && l + 1 < cap) l += (size_t)snprintf(out + l, cap - l, " %s", m.net_tag);
    if (m.joiners_optional && m.affects_gameplay && l + 1 < cap) snprintf(out + l, cap - l, " [joiners need not]");
}
/* ---------- the loader window (F1; the Mods page) as tabs - Mods | Keybinds | Hosting | Log | one tab per plugin that asked for one (API 2.33 ui_tab) ----------
 * The game thread owns the active tab (OvSnap.tab) and lists only its rows; a click on a tab, Q / E or the pad's LB / RB post OVC_MODS_TAB. A plugin tab calls
 * the plugin's draw callback inside the tab body (fault guard + stack recovery as for overlay callbacks); while it shows, the plugin has the keys (focus owner). */
typedef struct { int owner; char title[40]; void (*draw)(void *); void *user; } TabReg;
#define TABMAX 16
static TabReg g_tabs[TABMAX]; static int g_ntabs;                // guarded by g_cs
static int g_tab_owner = -1;                                     // render thread: the plugin whose tab was drawn this frame (-1 none)
extern "C" int ov_tab_register(int owner, const char *title, void (*draw)(void *), void *user) {
    if (!draw || owner < 0 || owner > 63 || !g_installed) return 1;
    EnterCriticalSection(&g_cs); int rc = 1, i = 0;
    while (i < g_ntabs && g_tabs[i].owner != owner) i++;
    if (i < TABMAX) { if (i == g_ntabs) g_ntabs++; g_tabs[i].owner = owner; snprintf(g_tabs[i].title, sizeof g_tabs[i].title, "%s", title && title[0] ? title : owner_name(owner)); g_tabs[i].draw = draw; g_tabs[i].user = user; rc = 0; }
    LeaveCriticalSection(&g_cs);
    if (!rc) LOG("overlay: plugin %s has a tab in the Mods window: '%s'", owner_name(owner), title ? title : "");
    return rc;
}
extern "C" int ov_tab_owner_at(int i, char *title, int cap) {
    if (!g_installed) return -1;
    EnterCriticalSection(&g_cs); int o = i >= 0 && i < g_ntabs ? g_tabs[i].owner : -1; if (o >= 0 && title && cap > 0) snprintf(title, (size_t)cap, "%s", g_tabs[i].title); LeaveCriticalSection(&g_cs);
    return o;
}
static int tab_drop_owner(int owner) {                              // caller holds g_cs
    int n = 0; for (int i = 0; i < g_ntabs; ) { if (g_tabs[i].owner == owner) { memmove(g_tabs + i, g_tabs + i + 1, sizeof(TabReg) * (size_t)(g_ntabs - i - 1)); g_ntabs--; n++; } else i++; }
    return n;
}
static void call_draw(const DrawReg &r);
static void lv_poll(void); static void draw_log_body(void); static int draw_form_body(const OvSnap &s, int tab);

// The rows of the active tab (keyboard / pad selection = s.sel, the mouse moving onto a row selects it). Kinds: overlay.h OvSnap.action_kind.
static void draw_rows(const OvSnap &s) {
    const ImVec4 grey(0.6f, 0.6f, 0.65f, 1), yellow(1.f, 0.85f, 0.35f, 1);
    static int last_sel = -1; int scroll_to = s.sel != last_sel && s.sel != g_hover_posted; if (s.sel != last_sel && s.sel != g_hover_posted) g_hover_posted = -1; last_sel = s.sel;
    float W = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < s.nactions; i++) {
        ImGui::PushID(1000 + i);
        int k = s.action_kind[i], item = k == 2 || k == 3 || k == 4 || k == 8 || ((k == 14 || k == 6 || k == 7) && i > 0 && s.action_kind[0] == 14);   // 3 / 4: plugin ui rows (API 2.27): a number gets a slider, a hint is grey; section items indented on the Mods tab
        if (k == 5) {                                                    // group header (Keybinds: the mod that owns the keys below)
            if (i) ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, i == s.sel ? yellow : ImVec4(0.55f, 0.8f, 1.f, 1.f)); ImGui::SeparatorText(s.action[i]); ImGui::PopStyleColor();
            if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.3f);
            ImGui::PopID(); continue;
        }
        if (k == 10 || k == 11) {                                        // big switch (Hosting: ON / OFF): a wide coloured button, the state in words
            const char *t = s.action[i] + 4; const char *dash = strstr(t, " - ");
            char head[48]; snprintf(head, sizeof head, "%.*s", dash ? (int)(dash - t) : (int)strlen(t), t);
            ImVec4 c = k == 10 ? ImVec4(0.18f, 0.55f, 0.25f, 1.f) : ImVec4(0.45f, 0.18f, 0.18f, 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button, c); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.x + 0.1f, c.y + 0.1f, c.z + 0.1f, 1.f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
            if (i == s.sel) ImGui::PushStyleColor(ImGuiCol_Border, yellow);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, i == s.sel ? 2.f : 0.f);
            ImGui::SetWindowFontScale(1.6f);
            if (ImGui::Button(head, ImVec2(ImGui::GetContentRegionAvail().x * 0.45f, 0.f))) post(OVC_MODS_ACT, i);
            row_hover(i, s.sel);
            ImGui::SetWindowFontScale(1.f);
            ImGui::PopStyleVar(); if (i == s.sel) ImGui::PopStyleColor(); ImGui::PopStyleColor(3);
            ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextWrapped("%s", dash ? dash + 3 : "");
            ImGui::TextDisabled("Click (or Enter / pad A) to switch. Players already here stay; F2 > Kick player removes one.");
            ImGui::Spacing(); ImGui::Separator();
            if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.3f);
            ImGui::PopID(); continue;
        }
        if (k == 9) {                                                    // grey text
            ImGui::PushStyleColor(ImGuiCol_Text, i == s.sel ? yellow : grey); ImGui::TextWrapped("%s", s.action[i]); ImGui::PopStyleColor();
            if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.3f);
            ImGui::PopID(); continue;
        }
        if (item) ImGui::Indent(16.f);
        if (i == s.sel) ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        if (k == 4) ImGui::PushStyleColor(ImGuiCol_Text, grey);
        bool hit;
        if (k == 14) {                                                   // a button: click, or select it with the keys / pad and press Enter / A
            const char *t = s.action[i]; while (*t == ' ') t++;
            if (i == s.sel) { ImGui::PushStyleColor(ImGuiCol_Border, yellow); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f); }
            hit = ImGui::Button(t);
            if (i == s.sel) { ImGui::PopStyleVar(); ImGui::PopStyleColor(); }
            row_hover(i, s.sel);
        } else if (k == 6 || k == 7 || k == 8) {                         // a full-width selectable row with widgets / columns drawn over it
            hit = ImGui::Selectable("##row", i == s.sel, ImGuiSelectableFlags_AllowOverlap);
            row_hover(i, s.sel);
            float x0 = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
            if (k == 8) {                                                // columns: label | when | key | warning
                const char *p = s.action[i]; static const float at[4] = { 0.f, 0.40f, 0.51f, 0.67f };
                for (int c = 0; c < 4 && p; c++) {
                    const char *e = strchr(p, '\t'); int n = e ? (int)(e - p) : (int)strlen(p);
                    ImGui::SameLine(x0 + W * at[c] + 4.f);
                    if (c == 3 && n) ImGui::TextColored(yellow, "%.*s", n, p); else if (c == 1) ImGui::TextDisabled("%.*s", n, p); else ImGui::Text("%.*s", n, p);
                    p = e ? e + 1 : NULL;
                }
            } else {                                                     // check box: the row and the box both toggle
                ImGui::SameLine(x0 + 4.f); bool b = k == 6;
                if (ImGui::Checkbox(s.action[i] + 4, &b)) hit = true;
            }
        } else {
            hit = ImGui::Selectable(s.action[i], i == s.sel, 0, ImVec2(k == 3 ? ImGui::GetContentRegionAvail().x * 0.55f : 0.f, 0.f));
            row_hover(i, s.sel);
        }
        if (k == 4) ImGui::PopStyleColor();
        if (i == s.sel) ImGui::PopStyleColor();
        if (hit) post(OVC_MODS_ACT, i);
        if (k == 3) {                                                    // the slider holds its own value while dragged (snapshots keep arriving); sent once on release
            static int drag_i = -1; static float drag_v;
            const double *n = s.action_num[i]; float v = drag_i == i ? drag_v : (float)n[0];
            ImGui::SameLine(); ImGui::SetNextItemWidth(-1.f);
            ImGui::SliderFloat("##uinum", &v, (float)n[1], (float)n[2], "%.6g");
            if (ImGui::IsItemActive()) { drag_i = i; drag_v = v; }
            if (ImGui::IsItemDeactivatedAfterEdit()) { char t[40]; snprintf(t, sizeof t, "%.9g", (double)v); post(OVC_UI_SET, i, (int)n[4], t); }
            if (!ImGui::IsItemActive() && drag_i == i) drag_i = -1;
        }
        if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.3f);
        if (item) ImGui::Unindent(16.f);
        ImGui::PopID();
    }
}
// What a mod holds, as coloured words (PAK / PLUGIN / ROWS / CONTENT / LUA) + its network tags in grey
static void holds_chips(const OvMod &m) {
    struct { int on; const char *t; ImVec4 c; } w[5] = {
        { m.has_pak, "PAK", ImVec4(0.50f, 0.75f, 1.f, 1) }, { m.has_plugin, "PLUGIN", ImVec4(0.80f, 0.62f, 1.f, 1) }, { m.has_rows, "ROWS", ImVec4(0.55f, 0.90f, 0.60f, 1) },
        { m.has_content, "CONTENT", ImVec4(0.95f, 0.80f, 0.50f, 1) }, { m.has_lua, "LUA", ImVec4(0.50f, 0.90f, 0.90f, 1) } };
    int first = 1;
    for (int k = 0; k < 5; k++) {
        if (!w[k].on) continue;
        if (!first) ImGui::SameLine(); first = 0;
        if (k == 0 && m.status == 0 && m.paks_total) ImGui::TextColored(w[k].c, "PAK %d/%d", m.paks_ok, m.paks_total);
        else if (k == 2 && m.rows_ops) ImGui::TextColored(w[k].c, "ROWS %d", m.rows_ops);
        else ImGui::TextColored(w[k].c, "%s", w[k].t);
    }
    char tags[96]; int l = 0; tags[0] = 0;
    if (!m.affects_gameplay) l += snprintf(tags + l, sizeof tags - (size_t)l, "[cosmetic] ");
    if (m.net_tag[0] && l < (int)sizeof tags - 24) l += snprintf(tags + l, sizeof tags - (size_t)l, "%s ", m.net_tag);
    if (m.joiners_optional && m.affects_gameplay && l < (int)sizeof tags - 24) snprintf(tags + l, sizeof tags - (size_t)l, "[joiners need not]");
    if (tags[0]) {
        if (!first) ImGui::SameLine(); ImGui::TextDisabled("%s", tags);
        if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::TextUnformatted(tags);
            ImGui::TextDisabled("[cosmetic]: left out of the LAN mod check. [host only] / [local only] / [vanilla visible] / [all need it]: who needs the mod (mod.json \"net\").");
            ImGui::TextDisabled("[joiners need not]: joiners can play without it (mod.json \"joiners_need\": false; left out of the LAN mod check)."); ImGui::EndTooltip(); }
    }
    if (m.plugin == 2) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1), "plugin BLOCKED (list it in Mods\\plugins.txt)");
}
static void draw_mods_tab(const OvSnap &s) {
    const ImVec4 green(0.45f, 0.9f, 0.5f, 1), red(1.f, 0.45f, 0.4f, 1), yellow(1.f, 0.85f, 0.35f, 1), grey(0.6f, 0.6f, 0.65f, 1);
    int on = 0, off = 0, skipped = 0;
    for (int i = 0; i < s.nmods; i++) { if (s.mods[i].status == 0) on++; else if (s.mods[i].status == 1) off++; else if (s.mods[i].status == 2) skipped++; }
    if (s.safe_mode) ImGui::TextColored(yellow, "SAFE MODE - no mods, no plugins, no extender hooks%s%s", s.safe_why[0] ? ": " : "", s.safe_why);
    ImGui::Text("%d mods loaded", on); ImGui::SameLine(); ImGui::TextDisabled("%d off, %d skipped", off, skipped);
    for (int i = 0; i < s.nwarn; i++) ImGui::TextColored(yellow, "! %s", s.warn[i]);
    if (s.pending_n) {
        ImGui::TextColored(yellow, "Waiting for a restart: %s", s.pending_text); ImGui::SameLine();
        if (ImGui::Button("Restart game now")) post(OVC_MODS_RESTART, 0);
    }
    // the mods: one row each, in load order (a later mod wins when two change the same thing)
    if (ImGui::BeginTable("mods", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 30.f * g_scale); ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthFixed, 230.f * g_scale);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 120.f * g_scale); ImGui::TableSetupColumn("Holds");
        ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 50.f * g_scale); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 160.f * g_scale);
        ImGui::TableHeadersRow();
        for (int i = 0; i < s.nmods; i++) {
            const OvMod &m = s.mods[i]; ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            { bool b = m.enabled != 0; if (s.safe_mode) ImGui::BeginDisabled(); if (ImGui::Checkbox("##on", &b)) post(OVC_MODS_ENABLE, i, b ? 1 : 0); if (s.safe_mode) ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.hot_ok ? "Switching a rows/content-only mod on takes effect now; everything else after a restart." : "Takes effect after a restart."); }
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(m.name);
            if (m.version[0] && strcmp(m.version, "0")) { ImGui::SameLine(); ImGui::TextDisabled("v%s", m.version); }
            if (m.id[0] && strcmp(m.id, m.name) && ImGui::IsItemHovered()) ImGui::SetTooltip("id: %s", m.id);
            ImGui::TableSetColumnIndex(2);
            if (m.effect == 3) ImGui::TextColored(green, "loaded now (live)");
            else if (m.effect == 1) ImGui::TextColored(yellow, "on after restart");
            else if (m.effect == 2) ImGui::TextColored(yellow, "off after restart");
            else if (m.status == 2) { ImGui::TextColored(red, "SKIPPED"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.reason[0] ? m.reason : "skipped"); }
            else ImGui::TextColored(m.status == 0 ? green : grey, "%s", m.status == 0 ? "loaded" : m.status == 1 ? "off" : "empty");
            if (m.nconf) { ImGui::SameLine(); ImGui::TextColored(yellow, "%d conflict%s", m.nconf, m.nconf == 1 ? "" : "s");
                if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); for (int k = 0; k < m.nconf && k < 3; k++) ImGui::TextUnformatted(m.conf[k]); if (m.nconf > 3) ImGui::TextDisabled("... %d in all (see mods.txt)", m.nconf); ImGui::EndTooltip(); } }
            ImGui::TableSetColumnIndex(3); holds_chips(m);
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("^")) post(OVC_MODS_MOVE, i, -1); ImGui::SameLine(); if (ImGui::SmallButton("v")) post(OVC_MODS_MOVE, i, 1);
            ImGui::TableSetColumnIndex(5);
            int tab = -1; for (int t = 0; t < g_ntabs; t++) if (!_stricmp(owner_name(g_tabs[t].owner), m.name)) tab = g_tabs[t].owner;
            if (tab >= 0) { if (ImGui::SmallButton("Open tab")) post(OVC_MODS_TAB, OVT_PLUGIN + tab); if (m.nsettings) ImGui::SameLine(); }
            if (m.nsettings && ImGui::SmallButton("Settings")) post(OVC_MODS_FORM, i);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // profile bar: switch to a saved on/off + order set, or save the current one under a name
    ImGui::Spacing();
    {
        static char nm[44]; static unsigned seen = 0xFFFFFFFFu; static int pick = -1;
        if (s.pf_serial != seen) { snprintf(nm, sizeof nm, "%s", s.pf_default); seen = s.pf_serial; pick = s.pf_current; }
        if (pick >= s.pf_n) pick = -1;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Profile"); ImGui::SameLine(); ImGui::SetNextItemWidth(200.f * g_scale);
        char cur[64]; snprintf(cur, sizeof cur, "%s", s.pf_current >= 0 && s.pf_current < s.pf_n ? s.pf_names[s.pf_current] : s.pf_n ? "(not a saved profile)" : "(none saved)");
        if (ImGui::BeginCombo("##pfpick", pick >= 0 ? s.pf_names[pick] : cur)) {
            for (int k = 0; k < s.pf_n; k++) if (ImGui::Selectable(s.pf_names[k], k == pick)) pick = k;
            ImGui::EndCombo();
        }
        ImGui::SameLine(); bool nosw = s.safe_mode || pick < 0 || pick == s.pf_current; if (nosw) ImGui::BeginDisabled();
        if (ImGui::Button("Switch")) post(OVC_PROFILE_SWITCH, pick);
        if (nosw) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Writes the profile's on/off + order set into Mods\\loadorder.txt (old file: .bak); it takes effect after a restart.");
        int exists = 0; for (int k = 0; k < s.pf_n; k++) if (!_stricmp(nm, s.pf_names[k])) exists = 1;
        ImGui::SameLine(0.f, 24.f * g_scale); ImGui::SetNextItemWidth(170.f * g_scale);
        ImGui::InputTextWithHint("##pfname", "new name", nm, sizeof nm);
        ImGui::SameLine(); bool nosave = s.safe_mode || !nm[0]; if (nosave) ImGui::BeginDisabled();
        if (ImGui::Button(exists ? "Update###pfsave" : "Save as###pfsave")) post(OVC_PROFILE_SAVE, 0, exists, nm);
        if (nosave) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", s.safe_mode ? "Safe mode: nothing is written." : exists ? "Overwrites this profile with the current on/off + order set (the old file is kept as .bak)." : "Saves the current on/off + order set as a new profile in Mods\\profiles (letters, digits, space, - _).");
        if (s.pf_msg[0]) ImGui::TextDisabled("%s", s.pf_msg);
    }
    // each mod's own rows (menu items, ui rows): one collapsible section per mod, under the reload row
    ImGui::Spacing(); ImGui::SeparatorText("Mod actions");
    draw_rows(s);
    // the rest only informs: open by itself when something is wrong
    int bad_plug = 0; for (int i = 0; i < s.nplug; i++) if (!s.plug_loaded[i] || s.plug_faults[i]) bad_plug = 1;
    int problem = s.split_assets || s.pak_conflicts_real || s.ncconf || s.c_invalid || bad_plug;
    ImGui::Spacing();
    if (problem) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.20f, 0.25f, 0.33f, 1.f));
    bool det = ImGui::CollapsingHeader(problem ? "Details  (something needs a look)###mdet" : "Details: load order, conflicts, plugins###mdet");
    ImGui::PopStyleColor();
    if (det) {
        ImGui::TextWrapped("Load order: %s", s.order[0] ? s.order : "(none)");
        if (s.info[0]) { ImGui::PushStyleColor(ImGuiCol_Text, grey); ImGui::TextWrapped("%s", s.info); ImGui::PopStyleColor(); }
        ImGui::Text("Conflicts: files %d (%d differ), row edits %d, data assets %d, split assets %d", s.pak_conflicts, s.pak_conflicts_real, s.row_conflicts, s.obj_conflicts, s.split_assets);
        if (s.split_assets) { ImGui::SameLine(); ImGui::TextColored(red, "LIKELY CRASH"); }
        ImGui::Text("Content registry: %d file(s), %d entr%s in use, %d invalid, %d conflict(s)", s.c_files, s.c_entries, s.c_entries == 1 ? "y" : "ies", s.c_invalid, s.ncconf);
        for (int i = 0; i < s.ncconf; i++) ImGui::TextColored(yellow, "  %s", s.cconf[i]);
        if (!s.nplug) ImGui::TextDisabled("Plugins: none loaded");
        for (int i = 0; i < s.nplug; i++) {
            ImGui::Text("Plugin %s", s.plug_name[i]); ImGui::SameLine();
            if (!s.plug_loaded[i]) ImGui::TextColored(red, "not running (load failed or refused)");
            else if (s.plug_faults[i] >= 3) ImGui::TextColored(red, "SWITCHED OFF after 3 faults");
            else if (s.plug_faults[i]) ImGui::TextColored(yellow, "running, %d of 3 faults", s.plug_faults[i]);
            else ImGui::TextColored(green, "running, no faults");
        }
        ImGui::TextWrapped("Last reload: %s", s.reload_status[0] ? s.reload_status : "none yet (the Reload row, or a LanNative\\reload.txt file, reloads Mods\\*\\content and rows.json)");
    }
}
// the pad's shoulder buttons and B, read from the real XInputGetState (the game's import is the neutralised one): edges, render thread
static unsigned pad_edges(void) {
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st; static WORD was;
    if (!g_xi_orig || !g_focus) { was = 0; return 0; }
    memset(&st, 0, sizeof st); if (g_xi_orig(0, &st) != ERROR_SUCCESS) { was = 0; return 0; }
    WORD now = st.buttons, e = (WORD)(now & ~was); was = now; return e;
}
static void draw_mods(const OvSnap &s) {
    // tab order: Mods, Keybinds, Hosting, Log, then the plugins' tabs in registration order
    int ids[OVT_COUNT + TABMAX], n = 0; const char *names[OVT_COUNT + TABMAX];
    static const char *const fixed[OVT_COUNT] = { "Mods", "Keybinds", "Hosting", "Settings", "Log" };
    for (int t = 0; t < OVT_COUNT; t++) { ids[n] = t; names[n++] = fixed[t]; }
    for (int t = 0; t < g_ntabs; t++) {                                // the mods' tabs by title (registration order depends on when each mod loads)
        int k = n++; ids[k] = OVT_PLUGIN + g_tabs[t].owner; names[k] = g_tabs[t].title;
        while (k > OVT_COUNT && _stricmp(names[k - 1], names[k]) > 0) { int ti = ids[k]; ids[k] = ids[k - 1]; ids[k - 1] = ti; const char *tn = names[k]; names[k] = names[k - 1]; names[k - 1] = tn; k--; }
    }
    int at = -1; for (int i = 0; i < n; i++) if (ids[i] == s.tab) at = i;
    if (at < 0) { if (s.tab >= OVT_PLUGIN) post(OVC_MODS_TAB, OVT_MODS); at = 0; }          // the plugin's tab is gone (switched off, faulted): back to Mods
    // Q / E and the pad's LB / RB step through the tabs (not while typing); in a plugin tab Esc / B close the window (the plugin has the other keys)
    unsigned pe = pad_edges();
    int kq = (int)InterlockedExchange(&g_tabkey_q, 0), ke = (int)InterlockedExchange(&g_tabkey_e, 0), kesc = (int)InterlockedExchange(&g_tabkey_esc, 0);
    int step = ke - kq; if (pe & 0x0100) step--; if (pe & 0x0200) step++;
    if (step) { post(OVC_MODS_TAB, ids[((at + step) % n + n) % n]); LOG("overlay: tab key %+d (Q %d, E %d, pad 0x%04X)", step, kq, ke, pe & 0x0300); }
    if (s.tab >= OVT_PLUGIN && !ImGui::IsAnyItemActive() && (kesc || (pe & 0x2000))) { post(OVC_MODS_CLOSE, 0); LOG("overlay: %s closes the window (plugin tab)", kesc ? "Esc" : "pad B"); }
    static int last_tab = -1; int force = s.tab != last_tab; last_tab = s.tab;              // the game thread changed the tab (key, pad, ui_tab_show): show it
    int shown = -1;
    if (ImGui::BeginTabBar("lntabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < n; i++) {
            if (i == OVT_COUNT) { ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.25f, 0.22f, 0.40f, 0.86f)); }   // the mods' own tabs in another colour
            char lbl[64]; snprintf(lbl, sizeof lbl, "%s###t%d", names[i], ids[i]);
            if (ImGui::BeginTabItem(lbl, NULL, force && i == at ? ImGuiTabItemFlags_SetSelected : 0)) { shown = ids[i]; ImGui::EndTabItem(); }
            if (i == n - 1 && n > OVT_COUNT) ImGui::PopStyleColor();
        }
        ImGui::EndTabBar();
    }
    static int asked = -1;                                                                // a click on a tab: asked once (the game thread answers with s.tab)
    if (shown == s.tab) asked = -1; else if (shown >= 0 && !force && shown != asked) { asked = shown; post(OVC_MODS_TAB, shown); }
    g_tab_owner = -1;
    float foot = ImGui::GetTextLineHeightWithSpacing() + 4.f;
    ImGui::BeginChild("##tabbody", ImVec2(0, -foot));
    if (shown != s.tab) ImGui::TextDisabled("...");                                       // one tick until the game thread lists the new tab's rows
    else if (s.tab == OVT_MODS) draw_mods_tab(s);
    else if (s.tab == OVT_KEYS) draw_rows(s);
    else if (s.tab == OVT_HOSTING) {
        ImGui::TextDisabled("Hosting = other players can join this game. Joining someone else: F2 > Join / LAN games.");
        ImGui::Separator(); draw_rows(s);
    }
    else if (s.tab == OVT_SETTINGS) {
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.35f, 1.f), "The loader reads these when the game starts: every change here applies after a restart.");
        ImGui::TextDisabled("Saved in LanNative\\config.txt. Hosting, host mod download and max players are on the Hosting tab and apply at once.");
        ImGui::Separator();
        if (s.form_mod == -2) draw_form_body(s, 1); else ImGui::TextDisabled("...");
    }
    else if (s.tab == OVT_LOG) { lv_poll(); draw_log_body(); }
    else {
        int owner = s.tab - OVT_PLUGIN; DrawReg r = { -1, NULL, NULL };
        for (int t = 0; t < g_ntabs; t++) if (g_tabs[t].owner == owner) { r.owner = owner; r.draw = g_tabs[t].draw; r.user = g_tabs[t].user; }
        if (r.draw) { g_tab_owner = owner; call_draw(r); }
    }
    ImGui::EndChild();
    ImGui::TextDisabled(s.tab >= OVT_PLUGIN ? "Q / E or LB / RB: tabs   Esc / B / F1: close   F2: world" : "Q / E or LB / RB: tabs   Up/Down + Enter, or click   Esc / F1: close   F2: world   Pad: d-pad, A, B");
}

static int draw_form_body(const OvSnap &s, int tab);
static void draw_form(const OvSnap &s) {
    bool open = true;
    float fw = 520.f * g_scale, fx = 40.f + 1000.f * g_scale + 20.f; if (fx + fw > (float)g_cw - 10.f) fx = (float)g_cw - fw - 10.f; if (fx < 10.f) fx = 10.f;     // beside the Mods window when there is room, otherwise on top of its right edge
    ImGui::SetNextWindowPos(ImVec2(fx, 100.f), ImGuiCond_Appearing); ImGui::SetNextWindowSize(ImVec2(fw, 420.f * g_scale), ImGuiCond_Appearing); ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin(s.form_title[0] ? s.form_title : "Mod settings###lnform", &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) { if (!draw_form_body(s, 0)) open = false; }
    ImGui::End();
    if (!open) post(OVC_MODS_FORM, -1);
}
// the fields + Apply / Defaults (/ Close in the window); 0 = Close was clicked. tab = 1: inside the F1 Settings tab (the loader's own settings, form_mod -2)
static int draw_form_body(const OvSnap &s, int tab) {
    static char buf[OV_SETTINGS][128]; static unsigned seen = 0xFFFFFFFFu; static int seen_mod = -3; int keep = 1;
    if (s.form_serial != seen || s.form_mod != seen_mod) { for (int i = 0; i < s.nform; i++) snprintf(buf[i], sizeof buf[i], "%s", s.form[i].value); seen = s.form_serial; seen_mod = s.form_mod; }
    {
        ImGui::BeginChild("fields", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() * 3.f + 12.f)));     // the buttons below stay visible however many settings there are
        for (int i = 0; i < s.nform; i++) {
            const OvSetting &st = s.form[i]; ImGui::PushID(i); char *v = buf[i];
            ImGui::TextUnformatted(st.label);
            if (st.type == 0) { bool b = v[0] == '1' || !_stricmp(v, "true") || !_stricmp(v, "on") || !_stricmp(v, "yes"); if (ImGui::Checkbox("##v", &b)) snprintf(v, 128, "%s", b ? "1" : "0"); }
            else if (st.type == 3) {
                if (ImGui::BeginCombo("##v", v[0] ? v : "(choose)")) { for (int k = 0; k < st.nchoice; k++) if (ImGui::Selectable(st.choice[k], !strcmp(v, st.choice[k]))) snprintf(v, 128, "%s", st.choice[k]); ImGui::EndCombo(); }
            } else if (st.type == 1 || st.type == 2) {                    // numbers: digits only, - / + steps, clamped to the setting's range (gleamui.h, shared with plugins)
                gleam_number_text("##v", v, 128, st.has_min ? st.min : -1e18, st.has_max ? st.max : 1e18, st.type == 1, 220.f * g_scale);
                if (st.has_min || st.has_max) { ImGui::SameLine(); ImGui::TextDisabled("(%g .. %g)", st.has_min ? st.min : -1e9, st.has_max ? st.max : 1e9); }
            } else { ImGui::SetNextItemWidth(260.f * g_scale); ImGui::InputText("##v", v, 128); }
            if (st.help[0]) ImGui::TextDisabled("%s", st.help);
            ImGui::PopID(); ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::Separator();
        if (ImGui::Button("Apply")) {
            char out[1536]; size_t w = 0; out[0] = 0;
            for (int i = 0; i < s.nform; i++) { int n = snprintf(out + w, sizeof out - w, "%s\t%s\n", s.form[i].key, buf[i]); if (n < 0 || w + (size_t)n >= sizeof out) break; w += (size_t)n; }
            post(OVC_MODS_APPLY, s.form_mod, 0, out);
        }
        ImGui::SameLine(); if (ImGui::Button("Defaults")) for (int i = 0; i < s.nform; i++) snprintf(buf[i], sizeof buf[i], "%s", s.form[i].def);
        if (!tab) { ImGui::SameLine(); if (ImGui::Button("Close")) keep = 0; }
        ImGui::TextDisabled("%s", s.form_live ? "Changes apply immediately." : "Changes apply after a restart.");
        if (s.form_msg[0]) { ImGui::PushStyleColor(ImGuiCol_Text, s.form_ok ? ImVec4(0.45f, 0.9f, 0.5f, 1) : ImVec4(1.f, 0.45f, 0.4f, 1)); ImGui::TextWrapped("%s", s.form_msg); ImGui::PopStyleColor(); }
    }
    return keep;
}
static int guarded_draw(void (*draw)(void *), void *user, unsigned long *code) {
    __try { draw(user); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return 0; }
}
static int unbalanced(const ImGuiErrorRecoveryState &a, const ImGuiErrorRecoveryState &b) {   // entries b has on its stacks beyond a
    const short *pa = &a.SizeOfWindowStack, *pb = &b.SizeOfWindowStack; int n = 0;
    for (size_t i = 0; i < sizeof(ImGuiErrorRecoveryState) / sizeof(short); i++) if (pb[i] > pa[i]) n += pb[i] - pa[i];
    return n;
}
// One plugin callback: ImGui's stacks are put back to what they were before the call (a Begin without End, a pushed style, an exception half way through a widget),
// so a plugin cannot corrupt the loader's windows or the plugins drawn after it.
static void call_draw(const DrawReg &r) {
    if (g_alive && !g_alive(r.owner)) return;
    // Frames run while ANY plugin holds a mode; a plugin that holds none is drawn only while a loader menu is open,
    // so a window that does not track its own visibility no longer shows up because another plugin keeps a HUD.
    if (!g_vis && r.owner >= 0 && r.owner < 64 && !g_mode[r.owner]) return;
    ImGuiErrorRecoveryState before, after; ImGui::ErrorRecoveryStoreState(&before);
    unsigned long code = 0; int ok = guarded_draw(r.draw, r.user, &code);
    if (!ok && g_fault) g_fault(r.owner, code);
    ImGui::ErrorRecoveryStoreState(&after); int n = unbalanced(before, after);
    if (n > 0 || !ok) {
        ImGui::ErrorRecoveryTryToRecoverState(&before);
        static unsigned seen[64];                                    // one line the first time, then every 1000th
        if (n > 0 && r.owner >= 0 && r.owner < 64 && (++seen[r.owner] == 1 || seen[r.owner] % 1000 == 0)) LOG("overlay: plugin %s left %d unbalanced ImGui stack entr%s%s - recovered (%u time(s) so far)", owner_name(r.owner), n, n == 1 ? "y" : "ies", ok ? "" : " after an exception", seen[r.owner]);
    }
}
// windows that were drawn this frame and have no owner yet belong to `owner` (-1 the loader): the focus owner is looked up from them (window id -> owner)
static void tag_windows(int owner) {
    ImGuiContext &g = *GImGui;
    for (int i = 0; i < g.Windows.Size; i++) {
        ImGuiWindow *wn = g.Windows[i]; if (wn->LastFrameActive != g.FrameCount) continue;
        if (g_win_frame.GetInt(wn->ID, -1) == g.FrameCount) continue;
        g_win_frame.SetInt(wn->ID, g.FrameCount); g_win_owner.SetInt(wn->ID, owner + 2);
    }
}
// who gets the keys and the pad this frame: the plugin whose window has ImGui's focus, if that plugin takes keys (ABS_OV_KEYS / BLOCK_GAME); else the loader menu
static void update_focus_owner(void) {
    ImGuiContext &g = *GImGui; int owner = -2;
    for (ImGuiWindow *wn = g.NavWindow; wn; wn = wn->ParentWindow)
        if (wn->LastFrameActive == g.FrameCount && g_win_frame.GetInt(wn->ID, -1) == g.FrameCount) { owner = g_win_owner.GetInt(wn->ID, 1) - 2; break; }
    if (owner >= 0 && (owner > 63 || !(g_mode[owner] & (OVM_KEYS | OVM_BLOCK)))) owner = g_vis ? -1 : -2;   // a HUD / mouse-only plugin window does not take the keys
    if (owner == -2 && g_vis) owner = -1;
    if (owner == -1 && g_tab_owner >= 0 && g_vis) owner = g_tab_owner;     // the loader window shows a plugin's tab
    LONG old = InterlockedExchange(&g_focus_owner, owner);
    if (old != owner) LOG("overlay: focus %s", owner >= 0 ? owner_name(owner) : owner == -1 ? "loader menu" : "nobody");
}
/* ---------- log window: the last lines of lannative.log, filter + "errors only", mod tags; read-only. Drawn beside the loader's
 * window while a loader menu is open (F2 row "Log window"); the file is read here on the render thread, incrementally, twice a second while shown. ---------- */
#include "logview.h"
static LogView g_lv; static wchar_t g_lv_path[MAX_PATH]; static volatile LONG g_lv_shown; static ULONGLONG g_lv_next; static long long g_lv_off = -1;
static char g_lv_filter[64]; static bool g_lv_err;
extern "C" void ov_log_window(const wchar_t *path, int on) {
    if (path) { wcsncpy(g_lv_path, path, MAX_PATH - 1); g_lv_path[MAX_PATH - 1] = 0; }
    InterlockedExchange(&g_lv_shown, on); g_lv_next = 0;
    LOG("overlay: log window %s", on ? "shown (lannative.log, last 3000 lines)" : "hidden");
}
extern "C" int ov_log_window_shown(void) { return (int)g_lv_shown; }
extern "C" int ov_text_input(void) { return (int)g_text_input; }  // 1 = an overlay text box has the keyboard (the menus leave the keys alone)
static void lv_poll(void) {
    ULONGLONG now = GetTickCount64(); if (now < g_lv_next || !g_lv_path[0]) return; g_lv_next = now + 500;
    if (!g_lv.l && !lv_init(&g_lv)) return;
    HANDLE h = CreateFileW(g_lv_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; int fresh = 0;
    if (GetFileSizeEx(h, &sz)) {
        if (g_lv_off < 0 || sz.QuadPart < g_lv_off) { lv_clear(&g_lv); g_lv_off = sz.QuadPart > 512 * 1024 ? sz.QuadPart - 512 * 1024 : 0; fresh = g_lv_off > 0; }   // first look or a restarted log
        static char buf[64 * 1024];
        while (g_lv_off < sz.QuadPart) {
            LARGE_INTEGER at; at.QuadPart = g_lv_off; DWORD got = 0;
            if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN) || !ReadFile(h, buf, sizeof buf, &got, NULL) || !got) break;
            const char *p = buf; DWORD n = got;
            if (fresh) { const char *nl = (const char *)memchr(buf, '\n', got); if (!nl) { g_lv_off += got; continue; } n -= (DWORD)(nl + 1 - buf); p = nl + 1; fresh = 0; }   // started mid-line: skip to the next line
            lv_feed(&g_lv, p, n); g_lv_off += got;
        }
    }
    CloseHandle(h);
}
static void draw_log_body(void) {                                  // the Mods window's Log tab, and the separate log window
        ImGui::SetNextItemWidth(260.f * g_scale); if (ImGui::InputTextWithHint("##lvf", "filter: text or mod name", g_lv_filter, sizeof g_lv_filter)) LOG("overlay: log filter = '%s'", g_lv_filter);
        ImGui::SameLine(); if (ImGui::Checkbox("Errors only", &g_lv_err)) LOG("overlay: log window errors only = %s", g_lv_err ? "on" : "off");
        static int idx[LV_MAX]; int m = 0;
        for (int i = 0; i < g_lv.n; i++) if (lv_match(lv_at(&g_lv, i), g_lv_filter, g_lv_err)) idx[m++] = i;
        ImGui::SameLine(); ImGui::TextDisabled("%d of the last %d lines", m, g_lv.n);
        ImGui::BeginChild("##lvl", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        bool at_end = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.f;
        ImGuiListClipper clip; clip.Begin(m);
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            const LvLine *l = lv_at(&g_lv, idx[r]); if (!l) continue;
            if (l->tag[0]) { ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.f, 1.f), "[%s]", l->tag); ImGui::SameLine(); }
            if (l->err) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.40f, 1.f), "%s", l->text); else ImGui::TextUnformatted(l->text);
        }
        if (at_end) ImGui::SetScrollHereY(1.f);                      // follows new lines while scrolled to the bottom
        ImGui::EndChild();
}
static void draw_log(int w, int h) {
    bool open = true; float ww = 900.f * g_scale, wh = 520.f * g_scale; if (ww > w - 40) ww = (float)(w - 40); if (wh > h - 40) wh = (float)(h - 40);
    ImGui::SetNextWindowPos(ImVec2((float)w - ww - 20.f, (float)h - wh - 20.f), ImGuiCond_Appearing); ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.90f);
    if (ImGui::Begin("Log - lannative.log###lnlog", &open, ImGuiWindowFlags_NoSavedSettings)) draw_log_body();
    ImGui::End();
    if (!open) ov_log_window(NULL, 0);
}

static void draw_ui(int w, int h) {
    const OvSnap &s = g_cur;
    g_cw = w; g_ch = h;
    static int last_kind = OV_NONE;
    if (s.kind != OV_NONE) {                                         // the loader's own windows first, balanced by construction
        bool open = true;
        float ww = s.kind == OV_MODS ? 1000.f * g_scale : 560.f * g_scale, wh = s.kind == OV_MODS ? 620.f * g_scale : 470.f * g_scale;
        if (ww > w - 40) ww = (float)(w - 40); if (wh > h - 40) wh = (float)(h - 40);
        ImGuiCond cond = s.kind != last_kind ? ImGuiCond_Always : ImGuiCond_Appearing;   // F1 <-> F2 share one window: each page gets its own size and place
        ImGui::SetNextWindowPos(ImVec2(s.kind == OV_MODS ? 40.f : 60.f, s.kind == OV_MODS ? 40.f : 80.f), cond);
        ImGui::SetNextWindowSize(ImVec2(ww, wh), cond);
        ImGui::SetNextWindowBgAlpha(0.88f);
        const char *name = s.kind == OV_MODS ? "Gleam Loader  (F1)###lnwin" : "World  (F2)###lnwin";   // F1 = the loader window, F2 = the world menu   // product name; the ###id stays
        // one clear highlight: the selected row (keys, pad, or the row the mouse moved onto) is strong, a row the cursor only rests on is faint
        // (ImGui's default style paints hover brighter than selection, so a resting mouse looked like a second selection - UX-B1 key_b05)
        const ImVec4 hl = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
        ImGui::PushStyleColor(ImGuiCol_Header, hl); ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(hl.x, hl.y, hl.z, hl.w * 0.30f));
        if (ImGui::Begin(name, &open, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse | (s.kind == OV_MODS && s.form_mod >= 0 ? ImGuiWindowFlags_NoBringToFrontOnFocus : 0))) {   // an open settings form stays above the page
            if (g_notice[0]) { ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.78f, 0.25f, 1.f)); ImGui::TextWrapped("HOST NOTICE  %s", g_notice); ImGui::PopStyleColor(); ImGui::Separator(); }   // a joiner sees what the host's mods announce
            if (s.kind == OV_MODS) draw_mods(s); else if (s.kind == OV_MENU) draw_menu(s); else draw_lines(s);
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        if (!open) post(s.kind == OV_MODS ? OVC_MODS_CLOSE : OVC_TOGGLE, 0);   // the X of the loader window closes it (OVC_TOGGLE = the world key)
        if (s.kind == OV_MODS && s.form_mod >= 0) draw_form(s);
        if (g_lv_shown) { lv_poll(); draw_log(w, h); }               // beside the loader's window, only while a loader menu is open
    }
    last_kind = s.kind;
    tag_windows(-1);
    DrawReg list[DRAWMAX]; int n = g_nreg; memcpy(list, g_reg, sizeof(DrawReg) * (size_t)n);   // a callback may register or unregister: iterate a copy, in registration order
    for (int i = 0; i < n; i++) { call_draw(list[i]); tag_windows(list[i].owner); }
    update_focus_owner();
}

/* ---------- Present ---------- */
static void make_rtv(void) {
    ID3D11Texture2D *bb = NULL; if (FAILED(g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td); D3D11_RENDER_TARGET_VIEW_DESC rd; memset(&rd, 0, sizeof rd); rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Format = td.Format;
    if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; else if (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    HRESULT hr = g_dev->CreateRenderTargetView(bb, &rd, &g_rtv); bb->Release();
    LOG("overlay: back buffer %ux%u format %d -> render target view %s (0x%08lX)", td.Width, td.Height, (int)td.Format, SUCCEEDED(hr) ? "ok" : "FAILED", (unsigned long)hr);
}
static int init(IDXGISwapChain *sc) {
    DXGI_SWAP_CHAIN_DESC d; if (FAILED(sc->GetDesc(&d))) return 0;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_dev)) || !g_dev) { LOG("overlay: swap chain has no D3D11 device - overlay off, text menu stays"); InterlockedExchange(&g_state, OV_DEAD); return 0; }
    ULONGLONG t0 = GetTickCount64();
    g_dev->GetImmediateContext(&g_ctx); g_sc = sc; g_hwnd = d.OutputWindow;
    IMGUI_CHECKVERSION(); g_ictx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); io.IniFilename = NULL; io.LogFilename = NULL;
    io.ConfigInputTrickleEventQueue = false;                         // a click and the virtual cursor position land in the same frame
    io.ConfigErrorRecoveryEnableAssert = false; io.ConfigErrorRecoveryEnableDebugLog = false; io.ConfigErrorRecoveryEnableTooltip = false;   // recovery is silent here; call_draw logs it
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_oldproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
    make_rtv(); g_imgui = 1;
    gcp_install();                                                   // no game hover under overlay windows (above)
    LOG("overlay: ImGui %s up on window %p (%ux%u, buffers %u, swap effect %d), WndProc subclassed (old %p), init took %llu ms", IMGUI_VERSION, (void *)g_hwnd, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferCount, (int)d.SwapEffect, (void *)g_oldproc, (unsigned long long)(GetTickCount64() - t0));
    return 1;
}
static void frame(IDXGISwapChain *sc) {
    if (!g_imgui) { if (!init(sc)) return; }
    if (sc != g_sc) return;                                          // another swap chain: leave it alone
    RECT r; if (!GetClientRect(g_hwnd, &r) || r.right <= 0 || r.bottom <= 0) return;
    int w = r.right, h = r.bottom;
    if (!g_rtv) make_rtv(); if (!g_rtv) return;
    EnterCriticalSection(&g_cs);
    if (g_alive) for (int o = 0; o < 64; o++) if (g_mode[o] && !g_alive(o)) ov_release_owner(o);   // a plugin that was switched off or unloaded keeps neither input nor callbacks
    if (g_pub_seq != g_cur_seq || g_cur.kind != g_pub.kind) { memcpy(&g_cur, &g_pub, sizeof g_cur); g_cur_seq = g_pub_seq; }
    g_scale = (float)h / 1080.f; if (g_scale < g_min_scale) g_scale = g_min_scale; if (g_scale > 3.f) g_scale = 3.f;
    ImGuiIO &io = ImGui::GetIO();
    io.FontGlobalScale = g_scale;
    io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NoMouseCursorChange) & ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);   // every frame: the pad belongs to the game and a plugin cannot turn navigation on
    if (InterlockedExchange(&g_clear, 0)) { io.ClearInputKeys(); io.ClearInputMouse(); }
    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame();
    if (g_vx < 0) { g_vx = w * 0.5f; g_vy = h * 0.5f; }
    // One cursor: when the game shows its OWN cursor (pause / inventory / any UMG menu) the overlay uses
    // that one - ImGui follows the OS cursor and draws none of its own. Only while the game hides it (gameplay: it pins the hidden cursor) the overlay draws
    // its virtual cursor, moved by raw mouse input.
    int want_cursor = g_vis || ((g_any & OVM_MOUSE) && !(g_any & OVM_HIDECURSOR)), os_cursor = 0;
    if (want_cursor && g_focus && !bg_on()) {                         // -LNBgInput: never the real OS cursor (it belongs to whoever uses the PC; its moves would "hover" rows)
        CURSORINFO ci; ci.cbSize = sizeof ci; POINT p;
        if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor && GetCursorPos(&p) && ScreenToClient(g_hwnd, &p) && p.x >= 0 && p.y >= 0 && p.x < w && p.y < h) {
            os_cursor = 1; g_vx = (float)p.x; g_vy = (float)p.y; InterlockedExchange(&g_dx, 0); InterlockedExchange(&g_dy, 0);
        }
    }
    if (!os_cursor) { g_vx += (float)InterlockedExchange(&g_dx, 0); g_vy += (float)InterlockedExchange(&g_dy, 0); }
    if (bg_on() && g_bgm_x >= 0) { g_vx = (float)g_bgm_x; g_vy = (float)g_bgm_y; }      // -LNBgInput: posted mouse messages place the cursor (lab-slot runs)
    if (g_vx < 0) g_vx = 0; if (g_vy < 0) g_vy = 0; if (g_vx > w - 1) g_vx = (float)(w - 1); if (g_vy > h - 1) g_vy = (float)(h - 1);
    io.MouseDrawCursor = want_cursor && !os_cursor;
    { LONG cm = !want_cursor ? 0 : os_cursor ? 2 : 1; if (InterlockedExchange(&g_cursor_mode, cm) != cm) LOG("overlay: cursor = %s", cm == 2 ? "the game's own cursor (it shows one): no second cursor drawn" : cm == 1 ? "the overlay's cursor (the game hides its own)" : "none"); }
    io.AddMousePosEvent(g_vx, g_vy);                                 // after the backend's own (pinned) OS cursor position
    ImGui::NewFrame();
    if (g_state == OV_STARTING) { InterlockedExchange(&g_state, OV_READY); LOG("overlay: first frame drawing - ImGui menus active"); }   // before the first callback, so overlay_info works in frame 1
    draw_ui(w, h);
    InterlockedExchange(&g_want_keys, io.WantCaptureKeyboard || io.WantTextInput);
    InterlockedExchange(&g_text_input, io.WantTextInput ? 1 : 0);  // ov_text_input: typing in a text box (log filter, settings form) is not menu navigation
    InterlockedExchange(&g_mouse_on_ui, io.WantCaptureMouse ? 1 : 0);  // gcp_hook: the game does not see the cursor over an overlay window
    ImGui::Render();
    ID3D11RenderTargetView *old_rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { 0 }; ID3D11DepthStencilView *old_dsv = NULL;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);   // the engine caches its bindings: put them back
    g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) if (old_rtv[i]) old_rtv[i]->Release();
    if (old_dsv) old_dsv->Release();
    LeaveCriticalSection(&g_cs);
    InterlockedIncrement(&g_frames);
}
static int frame_guarded(IDXGISwapChain *sc) { __try { frame(sc); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
// an exception inside frame() may leave g_cs held by this thread (it is recursive): release every level this thread owns
static void unlock_all(void) { if (g_cs.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId()) while (g_cs.RecursionCount > 0) LeaveCriticalSection(&g_cs); }
static void patch(void **vt, int i, void *fn, void **orig);
static void **g_vt;                                                    // the patched swap chain vtable (restored when a Present loop is found)
static DWORD g_pres_tid; static int g_pres_depth; static volatile LONG g_pres_reentry;
static HRESULT STDMETHODCALLTYPE hk_present_body(IDXGISwapChain *sc, UINT sync, UINT flags);
// Re-entry guard (Steam Play starts: a stack overflow in gameoverlayrenderer64): no Present hook chain calls back into this hook on the same thread.
// If one does, the nested call draws nothing and goes straight down the chain; past PRES_MAX_DEPTH it is a loop between two hooks, so this hook takes itself
// out of the vtable, switches the overlay off and returns without calling down again (one frame is not presented instead of the game dying).
#define PRES_MAX_DEPTH 6
static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    DWORD tid = GetCurrentThreadId();
    if (g_pres_depth > 0 && g_pres_tid == tid) {
        LONG n = InterlockedIncrement(&g_pres_reentry);
        if (n == 1) {
            HMODULE m = NULL; char who[MAX_PATH] = "?"; void *ra = _ReturnAddress();
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ra, &m) && m) GetModuleFileNameA(m, who, sizeof who);
            const char *b = strrchr(who, '\\');
            LOG("overlay: Present RE-ENTERED on the render thread (depth %d, called from %s+0x%llX) - another Present hook calls back into this one; nested calls draw nothing",
                g_pres_depth + 1, b ? b + 1 : who, m ? (unsigned long long)((char *)ra - (char *)m) : 0ull);
        }
        if (g_pres_depth >= PRES_MAX_DEPTH) {
            if (InterlockedExchange(&g_state, OV_DEAD) != OV_DEAD) {
                void *x;
                if (g_vt && g_vt[8] == (void *)hk_present) patch(g_vt, 8, (void *)o_present, &x);
                LOG("overlay: Present LOOP between two hooks (depth %d) - the overlay unhooks itself and stays off for this run, the text menu takes over", g_pres_depth + 1);
            }
            return S_OK;
        }
        g_pres_depth++; HRESULT hr = o_present(sc, sync, flags); g_pres_depth--;
        return hr;
    }
    g_pres_tid = tid; g_pres_depth = 1;
    HRESULT hr = hk_present_body(sc, sync, flags);
    g_pres_depth = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hk_present_body(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_present_calls);
    if (bg_on() && !g_bg_hwnd) bg_hook(sc);
    if ((g_vis || g_any) && g_state != OV_DEAD) {
        g_was_vis = 1;
        if (!frame_guarded(sc)) {
            unlock_all();
            LONG n = InterlockedIncrement(&g_faults);
            if (n >= 3) { InterlockedExchange(&g_state, OV_DEAD); InterlockedExchange(&g_vis, 0); }
            LOG("overlay: EXCEPTION in the overlay frame (%ld of 3)%s", n, n >= 3 ? " - overlay switched off for this run, the text menu takes over" : "");
        }
    } else if (g_was_vis) {                                          // hidden again: release whatever the overlay still holds
        g_was_vis = 0; InterlockedExchange(&g_mouse_on_ui, 0); InterlockedExchange(&g_text_input, 0);
        if (g_imgui) { EnterCriticalSection(&g_cs); ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse(); LeaveCriticalSection(&g_cs); }
    }
    return o_present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == g_sc && g_rtv) { g_rtv->Release(); g_rtv = NULL; }       // recreated on the next drawn frame
    return o_resize(sc, n, w, h, f, fl);
}
static void patch(void **vt, int i, void *fn, void **orig) {
    DWORD old; VirtualProtect(&vt[i], sizeof(void *), PAGE_EXECUTE_READWRITE, &old); *orig = vt[i]; vt[i] = fn; VirtualProtect(&vt[i], sizeof(void *), old, &old);
}
// A swap chain slot that points outside dxgi.dll / d3d11.dll was already patched by another module (for example a plugin with its own Present hook).
// Stacking a second patch on top of it can crash the game (stack overflow in the Steam overlay), so the loader keeps its text menus instead.
// A dxgi.dll proxy in the game folder (ReShade and similar) counts as dxgi.dll. Writes the module's file name to `who`; 1 = foreign.
static int slot_foreign(void *fn, char *who, int cap) {
    HMODULE m = NULL; char path[MAX_PATH] = "";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) || !m) { snprintf(who, cap, "no module (%p)", fn); return 1; }
    GetModuleFileNameA(m, path, sizeof path); const char *b = strrchr(path, '\\'); b = b ? b + 1 : path;
    snprintf(who, cap, "%s", b);
    return _stricmp(b, "dxgi.dll") != 0 && _stricmp(b, "d3d11.dll") != 0;
}

// The lock and the state, once (ov_prepare): plugins can register draw callbacks and modes from then on; nothing is hooked yet.
static INIT_ONCE g_once_prep = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK prepare_once(PINIT_ONCE, PVOID, PVOID *) {
    InitializeCriticalSection(&g_cs); g_req_tick = GetTickCount64();
    InterlockedExchange(&g_state, OV_STARTING); InterlockedExchange(&g_installed, 1);
    return TRUE;
}
extern "C" void ov_prepare(void) { InitOnceExecuteOnce(&g_once_prep, prepare_once, NULL, NULL); }
// Runs once, on whichever thread asks first (ov_request waits for it): the hook on the swap chain. overlayapi.inc calls it only once the game ticks:
// a probe device made during start-up (before the game's own device) patched the shared vtable before the Steam overlay hooked Present, and Steam's
// hook then recursed into a stack overflow 2 s later (seen in Steam Play starts).
static BOOL CALLBACK install_once(PINIT_ONCE, PVOID, PVOID *) {
    ov_prepare(); g_req_tick = GetTickCount64();
    HMODULE d3d = LoadLibraryW(L"d3d11.dll"); CreateDevSc_t create = d3d ? (CreateDevSc_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { LOG("overlay: d3d11.dll / D3D11CreateDeviceAndSwapChain not available - overlay off, text menu stays"); InterlockedExchange(&g_state, OV_DEAD); InterlockedExchange(&g_installed, 1); return TRUE; }
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"LanNativeOverlayProbe";
    RegisterClassExW(&wc); HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd); sd.BufferCount = 1; sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = w; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = NULL; ID3D11Device *dv = NULL; ID3D11DeviceContext *cx = NULL; D3D_FEATURE_LEVEL fl;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dv, &fl, &cx);
    if (FAILED(hr)) hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dv, &fl, &cx);
    char who8[MAX_PATH] = "", who13[MAX_PATH] = "";
    if (SUCCEEDED(hr) && sc && (slot_foreign((*(void ***)sc)[8], who8, sizeof who8) | slot_foreign((*(void ***)sc)[13], who13, sizeof who13))) {
        LOG("overlay: the swap chain's Present / ResizeBuffers are already patched by another module (%s / %s) - not stacking a second hook; overlay off, text menu stays", who8, who13);
        InterlockedExchange(&g_state, OV_DEAD);
    } else if (SUCCEEDED(hr) && sc) {
        void **vt = *(void ***)sc; g_vt = vt; patch(vt, 8, (void *)hk_present, (void **)&o_present); patch(vt, 13, (void *)hk_resize, (void **)&o_resize);
        LOG("overlay: Present / ResizeBuffers hooked (swap chain vtable %p)", (void *)vt);
    } else { LOG("overlay: probe device FAILED (0x%08lX) - overlay off, text menu stays", (unsigned long)hr); InterlockedExchange(&g_state, OV_DEAD); }
    if (sc) sc->Release(); if (cx) cx->Release(); if (dv) dv->Release();
    DestroyWindow(w); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    InterlockedExchange(&g_installed, 1);
    return TRUE;
}
extern "C" int ov_request(void) {                                    // idempotent; any thread; returns once the hook is in (or the attempt failed)
    InitOnceExecuteOnce(&g_once, install_once, NULL, NULL);
    return (int)g_state;
}
