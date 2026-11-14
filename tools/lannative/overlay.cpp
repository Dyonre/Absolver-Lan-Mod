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
#include "overlay.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
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
extern "C" void ov_release_owner(int owner) {
    if (owner < 0 || owner > 63 || !g_installed) return;
    EnterCriticalSection(&g_cs);
    unsigned old = g_mode[owner], was_mouse = (unsigned)(g_any & OVM_MOUSE); int dropped = 0;
    g_mode[owner] = 0; recompute_any();
    if (was_mouse && !(g_any & OVM_MOUSE)) InterlockedExchange(&g_clear, 1);
    for (int i = 0; i < g_nreg; ) { if (g_reg[i].owner == owner) { memmove(g_reg + i, g_reg + i + 1, sizeof(DrawReg) * (size_t)(g_nreg - i - 1)); g_nreg--; dropped++; } else i++; }
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
static void post(int cmd, int index) {                               // render thread; the caller holds g_cs (it is inside the frame)
    if (g_qn < CMDQ) { g_q[g_qn].cmd = cmd; g_q[g_qn].index = index; g_qn++; }
}
extern "C" void ov_publish(const OvSnap *s) {
    if (!g_installed) return;
    EnterCriticalSection(&g_cs);
    if (!s || s->kind == OV_NONE) { g_pub.kind = OV_NONE; InterlockedExchange(&g_vis, 0); }
    else {
        if (memcmp(&g_pub, s, sizeof g_pub) != 0) { memcpy(&g_pub, s, sizeof g_pub); g_pub_seq++; }
        InterlockedExchange(&g_vis, g_state == OV_DEAD ? 0 : 1);
    }
    LeaveCriticalSection(&g_cs);
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
    if (m == WM_KILLFOCUS || (m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE)) {   // focus lost: nothing is swallowed until it returns, ImGui forgets held keys and buttons
        InterlockedExchange(&g_focus, 0); InterlockedExchange(&g_clear, 1); InterlockedExchange(&g_rdx, 0); InterlockedExchange(&g_rdy, 0);
    } else if (m == WM_SETFOCUS || (m == WM_ACTIVATEAPP && w) || (m == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE)) InterlockedExchange(&g_focus, 1);
    if ((m == WM_KEYUP || m == WM_SYSKEYUP) && w < 256 && g_swallowed[w]) {         // the game never saw this key go down: it must not see it come up
        g_swallowed[w] = 0; if (feed) feed(h, m, w, l); return R_SWALLOW;
    }
    int down, btn = mouse_button(m, w, &down);
    if (down == 0 && (g_kept_btn & btn)) { g_kept_btn &= (unsigned char)~btn; if (feed) feed(h, m, w, l); return R_SWALLOW; }   // same for a mouse button
    LONG menu = g_vis, any = g_any;
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
    if (rc == ERROR_SUCCESS && state && (g_any & OVM_BLOCK) && g_focus) { xi_neutral(state); InterlockedIncrement(&g_xi_zeroed); }
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

/* ---------- drawing ---------- */
static void tt(const char *s) { if (ImGui::IsItemHovered() && s && s[0]) ImGui::SetTooltip("%s", s); }

static void draw_lines(const OvSnap &s) {
    for (int i = 0; i < s.count; i++) {
        if (i == 0) ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.f, 1.f), "%s", s.label[i]); else ImGui::TextUnformatted(s.label[i]);
    }
}
static void draw_menu(const OvSnap &s) {
    static int last_sel = -1; int scroll_to = s.sel != last_sel; last_sel = s.sel;
    ImGui::TextDisabled("%s", s.title);
    float foot = ImGui::GetFrameHeightWithSpacing() + 4.f;
    ImGui::BeginChild("rows", ImVec2(0, -foot), ImGuiChildFlags_Borders);
    for (int i = 0; i < s.count; i++) {
        if (ImGui::Selectable(s.label[i], i == s.sel)) post(OVC_MENU_SELECT, i);
        if (i == s.sel && scroll_to) ImGui::SetScrollHereY(0.5f);
    }
    ImGui::EndChild();
    if (s.has_back) { if (ImGui::Button("Back")) post(OVC_MENU_BACK, 0); ImGui::SameLine(); }
    if (ImGui::Button("Close")) post(OVC_TOGGLE, 0);
    ImGui::SameLine(); ImGui::TextDisabled("Up/Down + Enter, or click. F1 closes.");
}

static void content_text(const OvMod &m, char *out, size_t cap) {
    int w = 0; out[0] = 0;
    if (m.paks_total) w += snprintf(out + w, cap - (size_t)w, "%d/%d pak  ", m.paks_ok, m.paks_total);
    if (m.rows_ops) w += snprintf(out + w, cap - (size_t)w, "%d row edit(s)  ", m.rows_ops);
    if (m.content_files) w += snprintf(out + w, cap - (size_t)w, "%d content  ", m.content_files);
    if (m.plugin == 1) w += snprintf(out + w, cap - (size_t)w, "plugin  ");
    if (!m.affects_gameplay) snprintf(out + w, cap - (size_t)w, "[cosmetic]");
}
static void draw_mods(const OvSnap &s) {
    const ImVec4 green(0.45f, 0.9f, 0.5f, 1), red(1.f, 0.45f, 0.4f, 1), yellow(1.f, 0.85f, 0.35f, 1), grey(0.6f, 0.6f, 0.65f, 1);
    int on = 0, off = 0, skipped = 0;
    for (int i = 0; i < s.nmods; i++) { if (s.mods[i].status == 0) on++; else if (s.mods[i].status == 1) off++; else if (s.mods[i].status == 2) skipped++; }
    ImGui::Text("Safe mode:"); ImGui::SameLine();
    if (s.safe_mode) ImGui::TextColored(yellow, "ON - no mods, no plugins, no extender hooks%s%s", s.safe_why[0] ? ": " : "", s.safe_why); else ImGui::TextColored(green, "off");
    ImGui::Text("Mods: %d loaded, %d off, %d skipped", on, off, skipped);
    ImGui::TextWrapped("Load order: %s", s.order[0] ? s.order : "(none)");
    ImGui::Separator();
    for (int i = 0; i < s.nactions; i++) {                           // actions: the reload line, then one line per plugin section / item
        if (s.action_kind[i] == 0) { if (ImGui::Button(s.action[i])) post(OVC_MODS_ACT, i); }
        else {
            if (s.action_kind[i] == 2) ImGui::Indent(16.f);
            if (ImGui::Selectable(s.action[i], i == s.sel)) post(OVC_MODS_ACT, i);
            if (s.action_kind[i] == 2) ImGui::Unindent(16.f);
        }
    }
    if (s.nactions) ImGui::Separator();
    if (s.nwarn && ImGui::CollapsingHeader("Warnings", ImGuiTreeNodeFlags_DefaultOpen)) for (int i = 0; i < s.nwarn; i++) ImGui::TextColored(yellow, "%s", s.warn[i]), ImGui::Spacing();
    if (ImGui::CollapsingHeader("Mods", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("mods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.f * g_scale); ImGui::TableSetupColumn("Mod"); ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 60.f * g_scale); ImGui::TableSetupColumn("Content / reason");
        ImGui::TableHeadersRow();
        static const char *names[] = { "loaded", "off", "SKIPPED", "empty" };
        for (int i = 0; i < s.nmods; i++) {
            const OvMod &m = s.mods[i]; char c[200];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextColored(m.status == 0 ? green : m.status == 2 ? red : grey, "%s", names[m.status >= 0 && m.status < 4 ? m.status : 3]);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(m.name); if (m.id[0] && strcmp(m.id, m.name)) { ImGui::SameLine(); ImGui::TextDisabled("(%s)", m.id); }
            ImGui::TableSetColumnIndex(2); if (m.version[0] && strcmp(m.version, "0")) ImGui::TextUnformatted(m.version);
            ImGui::TableSetColumnIndex(3);
            if (m.status == 2) ImGui::TextColored(red, "%s", m.reason[0] ? m.reason : "skipped");
            else { content_text(m, c, sizeof c); ImGui::TextUnformatted(c); if (m.plugin == 2) { ImGui::SameLine(); ImGui::TextColored(red, "plugin BLOCKED (list it in Mods\\plugins.txt)"); } }
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Conflicts", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("files %d (%d differ), row edits %d, data assets %d, split assets %d", s.pak_conflicts, s.pak_conflicts_real, s.row_conflicts, s.obj_conflicts, s.split_assets);
        if (s.split_assets) { ImGui::SameLine(); ImGui::TextColored(red, "LIKELY CRASH"); }
    }
    if (ImGui::CollapsingHeader("Content registry", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%d file(s), %d entr%s in use, %d invalid, %d conflict(s)", s.c_files, s.c_entries, s.c_entries == 1 ? "y" : "ies", s.c_invalid, s.ncconf);
        for (int i = 0; i < s.ncconf; i++) ImGui::TextColored(yellow, "  %s", s.cconf[i]);
    }
    if (ImGui::CollapsingHeader("Plugins", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!s.nplug) ImGui::TextDisabled("none loaded");
        for (int i = 0; i < s.nplug; i++) {
            ImGui::TextUnformatted(s.plug_name[i]); ImGui::SameLine();
            if (!s.plug_loaded[i]) ImGui::TextColored(red, "not running (load failed or refused)");
            else if (s.plug_faults[i] >= 3) ImGui::TextColored(red, "SWITCHED OFF after 3 faults");
            else if (s.plug_faults[i]) ImGui::TextColored(yellow, "running, %d of 3 faults", s.plug_faults[i]);
            else ImGui::TextColored(green, "running, no faults");
        }
    }
    ImGui::Separator();
    ImGui::TextWrapped("Last reload: %s", s.reload_status[0] ? s.reload_status : "none yet (the Reload line of this page, or a LanNative\\reload.txt file, reloads Mods\\*\\content and rows.json)");
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
static void draw_ui(int w, int h) {
    const OvSnap &s = g_cur;
    ImGuiIO &io = ImGui::GetIO(); io.MouseDrawCursor = g_vis || ((g_any & OVM_MOUSE) && !(g_any & OVM_HIDECURSOR)); g_cw = w; g_ch = h;
    if (s.kind != OV_NONE) {                                         // the loader's own windows first, balanced by construction
        bool open = true;
        float ww = s.kind == OV_MODS ? 860.f * g_scale : 560.f * g_scale, wh = s.kind == OV_MODS ? 620.f * g_scale : 470.f * g_scale;
        if (ww > w - 40) ww = (float)(w - 40); if (wh > h - 40) wh = (float)(h - 40);
        ImGui::SetNextWindowPos(ImVec2(s.kind == OV_MODS ? 40.f : 60.f, s.kind == OV_MODS ? 40.f : 80.f), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(ww, wh), ImGuiCond_Appearing);
        ImGui::SetNextWindowBgAlpha(0.88f);
        const char *name = s.kind == OV_MODS ? "Mods###lnwin" : "LanNative###lnwin";
        if (ImGui::Begin(name, &open, ImGuiWindowFlags_NoSavedSettings)) {
            if (s.kind == OV_MODS) draw_mods(s); else if (s.kind == OV_MENU) draw_menu(s); else draw_lines(s);
        }
        ImGui::End();
        if (!open) post(OVC_TOGGLE, 0);
    }
    DrawReg list[DRAWMAX]; int n = g_nreg; memcpy(list, g_reg, sizeof(DrawReg) * (size_t)n);   // a callback may register or unregister: iterate a copy, in registration order
    for (int i = 0; i < n; i++) call_draw(list[i]);
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
    g_vx += (float)InterlockedExchange(&g_dx, 0); g_vy += (float)InterlockedExchange(&g_dy, 0);
    if (g_vx < 0) g_vx = 0; if (g_vy < 0) g_vy = 0; if (g_vx > w - 1) g_vx = (float)(w - 1); if (g_vy > h - 1) g_vy = (float)(h - 1);
    io.AddMousePosEvent(g_vx, g_vy);                                 // after the backend's own (pinned) OS cursor position
    ImGui::NewFrame();
    if (g_state == OV_STARTING) { InterlockedExchange(&g_state, OV_READY); LOG("overlay: first frame drawing - ImGui menus active"); }   // before the first callback, so overlay_info works in frame 1
    draw_ui(w, h);
    InterlockedExchange(&g_want_keys, io.WantCaptureKeyboard || io.WantTextInput);
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
static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_present_calls);
    if ((g_vis || g_any) && g_state != OV_DEAD) {
        g_was_vis = 1;
        if (!frame_guarded(sc)) {
            unlock_all();
            LONG n = InterlockedIncrement(&g_faults);
            if (n >= 3) { InterlockedExchange(&g_state, OV_DEAD); InterlockedExchange(&g_vis, 0); }
            LOG("overlay: EXCEPTION in the overlay frame (%ld of 3)%s", n, n >= 3 ? " - overlay switched off for this run, the text menu takes over" : "");
        }
    } else if (g_was_vis) {                                          // hidden again: release whatever the overlay still holds
        g_was_vis = 0;
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

// Runs once, on whichever thread asks first (ov_request waits for it): the lock, the state, and the hook on the swap chain.
static BOOL CALLBACK install_once(PINIT_ONCE, PVOID, PVOID *) {
    InitializeCriticalSection(&g_cs); g_req_tick = GetTickCount64();
    InterlockedExchange(&g_state, OV_STARTING);
    HMODULE d3d = LoadLibraryW(L"d3d11.dll"); CreateDevSc_t create = d3d ? (CreateDevSc_t)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { LOG("overlay: d3d11.dll / D3D11CreateDeviceAndSwapChain not available - overlay off, text menu stays"); InterlockedExchange(&g_state, OV_DEAD); InterlockedExchange(&g_installed, 1); return TRUE; }
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"LanNativeOverlayProbe";
    RegisterClassExW(&wc); HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd); sd.BufferCount = 1; sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = w; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = NULL; ID3D11Device *dv = NULL; ID3D11DeviceContext *cx = NULL; D3D_FEATURE_LEVEL fl;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dv, &fl, &cx);
    if (FAILED(hr)) hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dv, &fl, &cx);
    if (SUCCEEDED(hr) && sc) {
        void **vt = *(void ***)sc; patch(vt, 8, (void *)hk_present, (void **)&o_present); patch(vt, 13, (void *)hk_resize, (void **)&o_resize);
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
