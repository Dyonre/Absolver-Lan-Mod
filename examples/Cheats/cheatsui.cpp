// Cheats UI (Dear ImGui through the loader's overlay API): on loaders with mod tabs (API 2.33 ui_tab) the rows are a "Cheats" tab of the loader window, otherwise a
// window of its own. The window is drawn only while it is open (the loader runs overlay frames whenever any plugin holds an overlay mode).
// Input in the window: ABS_OV_DRAW | MOUSE | KEYS | BLOCK_GAME (the game gets no keys, mouse or pad). ImGui's own keyboard / gamepad navigation is off in the loader,
// so the rows have their own: Up / Down select, Left / Right change a value, Enter / Space use; the pad's D-pad / left stick, A use, B back / close (read with
// XInputGetState directly). Render thread only: values come from plugin.c through cheatsui.h, requests go back the same way (run on the game thread).
// Log: "cheats-ui: ...".
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "cheatsui.h"
#include "imgui.h"
#include "../../tools/lannative/gleamui.h"   // number fields shared with the loader's forms (0.8.0)
#include "imgui_internal.h"   // FindWindowByName: is a loader menu drawn this frame (it reads Up / Down / Enter with GetAsyncKeyState too)

static const AbsApi *A;
static volatile LONG g_open, g_opened_by;   // g_opened_by: serial of opens, so the render thread notices a fresh open
static int g_refused;
// 0.7.0 (loader API 2.33 ui_tab): the rows are a "Cheats" tab of the loader's Mods window instead of a window of their own. The window key / the F2 row open
// the loader window on that tab (ui_tab_show); the loader closes it (Esc, pad B, F2) and keeps Q / E / LB / RB for its tabs. Older loaders: the 0.6 window.
static int g_tab;                           // 1 = drawn as a loader tab
static volatile ULONGLONG g_tab_seen;       // last time the tab was drawn (cui_is_open in tab mode)

// binds the plugin's ImGui to the loader's context; 0 = the layouts differ (draw nothing)
static int bind(void) {
    AbsOverlayInfo i; memset(&i, 0, sizeof i); i.size = sizeof i;
    if (A->overlay_info(A, &i)) return 0;
    if (i.imgui_version_num != IMGUI_VERSION_NUM || i.sizeof_imgui_io != (int)sizeof(ImGuiIO) || i.sizeof_imgui_style != (int)sizeof(ImGuiStyle) ||
        i.sizeof_imgui_drawvert != (int)sizeof(ImDrawVert) || i.sizeof_imgui_drawidx != (int)sizeof(ImDrawIdx) || i.sizeof_imgui_wchar != (int)sizeof(ImWchar)) {
        if (!g_refused) { g_refused = 1; A->log(A, "cheats-ui: ImGui layout differs from the loader's - the Cheats window is not drawn"); }
        return 0;
    }
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)i.imgui_alloc, (ImGuiMemFreeFunc)i.imgui_free, i.imgui_alloc_user);
    ImGui::SetCurrentContext((ImGuiContext *)i.imgui_context);
    return 1;
}
static float g_sc = 1.f;   // io.FontGlobalScale: the loader scales the font with the window height, sizes in pixels are scaled with it
static inline float U(float v) { return v * g_sc; }

// ---- the rows (one list, grouped)
enum { RK_HEAD, RK_TOGGLE, RK_ACT, RK_HEAL, RK_VAL, RK_ITEMS };
enum { V_FLY, V_PLAYER, V_NPC, V_NPCMAX, V_SPEED };
struct Row { int kind, id; const char *text; };
static const Row ROWS[] = {
    { RK_HEAD, 0, "Player" }, { RK_TOGGLE, 0 }, { RK_TOGGLE, 1 }, { RK_TOGGLE, 2 }, { RK_TOGGLE, 3 }, { RK_TOGGLE, 4 }, { RK_VAL, V_FLY, "Fly speed" },
    { RK_HEAL, 0, "Heal me" }, { RK_ACT, W_EMOTES },
    { RK_HEAD, 0, "World" }, { RK_ACT, W_TPAIM }, { RK_ACT, W_SAVESPOT }, { RK_ACT, W_LOADSPOT }, { RK_ACT, W_DOOR }, { RK_ACT, W_ALTAR },
    { RK_VAL, V_PLAYER, "Player #" }, { RK_ACT, W_GOTO }, { RK_ACT, W_BRING }, { RK_ACT, W_BRINGALL },
    { RK_HEAD, 0, "NPCs" }, { RK_VAL, V_NPC, "NPC type" }, { RK_VAL, V_NPCMAX, "Max NPCs near me" }, { RK_ACT, W_SPAWN }, { RK_ACT, W_AGGRO }, { RK_ACT, W_KILL }, { RK_ACT, W_CLEAR },
    { RK_HEAD, 0, "Game speed" }, { RK_VAL, V_SPEED, "Speed" }, { RK_ACT, W_SPEED, "Apply this speed" },
    { RK_HEAD, 0, "Debug" }, { RK_ACT, W_DEBUG },
    { RK_HEAD, 0, "Items" }, { RK_ITEMS, 0, "Add items to the inventory..." },
};
enum { NROWS = (int)(sizeof ROWS / sizeof ROWS[0]) };
static int g_sel = 1, g_scroll_sel, g_view_items, g_layout_logged;
static int g_pick_req, g_pick_open;          /* NPC picker: open it next frame (Enter / pad A on its row); it is open (rows ignore Up / Down / Enter meanwhile) */

static const char *row_label(int i) {
    const Row &r = ROWS[i];
    if (r.text) return r.text;
    if (r.kind == RK_TOGGLE) return cheats_label(r.id);
    return cheats_world_label(r.id);
}
static int row_host_only(int i) { const Row &r = ROWS[i]; return (r.kind == RK_TOGGLE && (r.id == 1 || r.id == 4)) || (r.kind == RK_ACT && cheats_world_host_only(r.id)); }
static void row_value(int i, char *out, int cap) {
    const Row &r = ROWS[i]; out[0] = 0;
    if (r.kind == RK_TOGGLE) snprintf(out, (size_t)cap, "%s", cheats_is_on(r.id) ? "ON" : "off");
    else if (r.kind == RK_VAL) {
        switch (r.id) {
        case V_FLY: snprintf(out, (size_t)cap, "%.0f uu/s", cheats_speed()); break;
        case V_PLAYER: { WorldView v; cheats_world_view(&v); int k = cheats_get_int(0) - 1; snprintf(out, (size_t)cap, "#%d %s%s", k + 1, k < v.nplayers ? v.player[k] : "-", k < v.nplayers && v.is_me[k] ? " (you)" : ""); break; }
        case V_NPC: snprintf(out, (size_t)cap, "%s", cheats_npc_label(cheats_get_int(1))); break;
        case V_NPCMAX: snprintf(out, (size_t)cap, "%d", cheats_get_int(2)); break;
        case V_SPEED: snprintf(out, (size_t)cap, "%.1fx", cheats_dilation()); break;
        }
    }
}
static void row_adjust(int i, int dir, const char *src) {
    const Row &r = ROWS[i]; if (r.kind != RK_VAL || !dir) return;
    switch (r.id) {
    case V_FLY: { float v = cheats_speed() + 100.f * dir; cheats_set_speed(v < 100.f ? 100.f : v > 4000.f ? 4000.f : v); break; }
    case V_PLAYER: { int v = cheats_get_int(0) + dir; cheats_set_int(0, v < 1 ? 1 : v > 8 ? 8 : v); break; }
    case V_NPC: { int n = cheats_npc_count(), v = (cheats_get_int(1) + dir + n) % n; cheats_set_int(1, v); break; }
    case V_NPCMAX: { int v = cheats_get_int(2) + dir; cheats_set_int(2, v < 1 ? 1 : v > 40 ? 40 : v); break; }
    case V_SPEED: { int t = (int)(cheats_dilation() * 10.f + 0.5f) + dir; cheats_set_dilation((t < 1 ? 1 : t > 30 ? 30 : t) / 10.f); break; }
    }
    char v[96]; row_value(i, v, sizeof v); A->log(A, "cheats-ui: %s: %s %s -> %s", src, row_label(i), dir < 0 ? "-" : "+", v);
}
static void row_use(int i, const char *src) {
    const Row &r = ROWS[i];
    A->log(A, "cheats-ui: %s: use row %d '%s'%s", src, i, row_label(i), r.kind == RK_TOGGLE ? (cheats_is_on(r.id) ? " (ON -> off)" : " (off -> ON)") : "");
    switch (r.kind) {
    case RK_TOGGLE: cheats_request_toggle(r.id); break;
    case RK_ACT: cheats_world(r.id); break;
    case RK_HEAL: cheats_request_heal(); break;
    case RK_ITEMS: g_view_items = 1; break;
    case RK_VAL: if (r.id == V_NPC) { g_pick_req = 1; break; } row_adjust(i, 1, src); break;   /* the NPC row opens its picker (keys / pad pick in it) */
    }
}
static int row_step(int from, int dir) { for (int i = from + dir; i >= 0 && i < NROWS; i += dir) if (ROWS[i].kind != RK_HEAD) return i; return from; }

// ---- gamepad (first connected pad), read directly: the game's own XInputGetState import is the one the loader neutralises under BLOCK_GAME
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } PadGamepad;
typedef struct { DWORD dwPacketNumber; PadGamepad Gamepad; } PadState;
typedef DWORD(WINAPI *PadGet_t)(DWORD, PadState *);
static PadGet_t g_padget; static int g_pad_tried;
enum { P_UP, P_DOWN, P_LEFT, P_RIGHT, P_A, P_B, P_N };
static ULONGLONG g_pad_t[P_N]; static int g_pad_down[P_N], g_pad_sync = 1;
static void pad_load(void) {
    if (g_pad_tried) return; g_pad_tried = 1;
    const wchar_t *dll[] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
    for (int k = 0; k < 3 && !g_padget; k++) { HMODULE m = GetModuleHandleW(dll[k]); if (!m) m = LoadLibraryW(dll[k]); if (m) g_padget = (PadGet_t)GetProcAddress(m, "XInputGetState"); }
    A->log(A, "cheats-ui: gamepad: XInputGetState %s", g_padget ? "found" : "NOT found (no pad navigation)");
}
// presses this frame: a new press, then every 110 ms after 380 ms held (directions only)
static void pad_poll(int out[P_N]) {
    memset(out, 0, sizeof(int) * P_N); pad_load(); if (!g_padget) return;
    PadState s; int cur[P_N] = { 0 };
    for (DWORD u = 0; u < 4; u++) {
        memset(&s, 0, sizeof s); if (g_padget(u, &s) != ERROR_SUCCESS) continue;
        WORD b = s.Gamepad.wButtons; SHORT y = s.Gamepad.sThumbLY, x = s.Gamepad.sThumbLX;
        cur[P_UP] |= (b & 0x0001) || y > 16000; cur[P_DOWN] |= (b & 0x0002) || y < -16000; cur[P_LEFT] |= (b & 0x0004) || x < -16000; cur[P_RIGHT] |= (b & 0x0008) || x > 16000;
        cur[P_A] |= (b & 0x1000) != 0; cur[P_B] |= (b & 0x2000) != 0;
    }
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < P_N; k++) {
        if (g_pad_sync) { g_pad_down[k] = cur[k]; g_pad_t[k] = now + 380; continue; }   // a button already held when the window opened does nothing
        if (cur[k] && !g_pad_down[k]) { out[k] = 1; g_pad_t[k] = now + 380; }
        else if (cur[k] && k <= P_RIGHT && now >= g_pad_t[k]) { out[k] = 1; g_pad_t[k] = now + 110; }
        g_pad_down[k] = cur[k];
    }
    g_pad_sync = 0;
}

// ---- Add items view (the 0.4 tab, now a page of the window: Back / Esc / pad B return to the rows)
static char g_query[96]; static int g_kinds = CAT_ALL;
static char g_isel[200]; static int g_count = 1, g_var, g_init, g_icur = -1, g_iscroll; static bool g_equip;
static int g_idx[CAT_MAX], g_nidx; static unsigned g_list_serial = ~0u; static char g_list_query[96]; static int g_list_kinds = -1;
static int g_asked, g_test_tab, g_test_select, g_test_done;

static void view_items(int up, int down, int use) {
    if (!g_init) { g_init = 1; g_count = cheats_default_count(); g_equip = cheats_default_equip() != 0; }
    int st = cheats_catalog_state();
    if (st == CU_BUILD_NONE && !g_asked) { g_asked = 1; cheats_request_catalog(); st = cheats_catalog_state(); }   // built when the page opens, never per tick
    if (ImGui::Button("< Back")) { g_view_items = 0; A->log(A, "cheats-ui: mouse: items page -> rows"); return; }
    ImGui::SameLine(); if (ImGui::Button("Refresh")) cheats_request_catalog();
    ImGui::SameLine(); ImGui::SetNextItemWidth(U(240.f)); ImGui::InputTextWithHint("##q", "search: name, slot, kind, mod", g_query, sizeof g_query);
    bool g = (g_kinds & CAT_GEAR) != 0, w = (g_kinds & CAT_WEAPON) != 0, p = (g_kinds & CAT_POWER) != 0;
    if (ImGui::Checkbox("Gear", &g)) g_kinds = (g_kinds & ~CAT_GEAR) | (g ? CAT_GEAR : 0);
    ImGui::SameLine(); if (ImGui::Checkbox("Weapons", &w)) g_kinds = (g_kinds & ~CAT_WEAPON) | (w ? CAT_WEAPON : 0);
    ImGui::SameLine(); if (ImGui::Checkbox("Powers", &p)) g_kinds = (g_kinds & ~CAT_POWER) | (p ? CAT_POWER : 0);
    if (st == CU_BUILD_RUNNING) { ImGui::TextDisabled("Reading the game's items..."); return; }
    if (st == CU_BUILD_NONE) { ImGui::TextDisabled("No list yet (a world has to be loaded)."); return; }

    Catalog *cat = cheats_catalog_lock();
    unsigned ser = cheats_catalog_serial();
    if (ser != g_list_serial || g_list_kinds != g_kinds || strcmp(g_list_query, g_query)) {
        g_nidx = cat_filter(cat, g_query, g_kinds, g_idx, CAT_MAX); g_list_serial = ser; g_list_kinds = g_kinds; snprintf(g_list_query, sizeof g_list_query, "%s", g_query); g_icur = -1;
    }
    if (g_nidx && (up || down)) {   // keys / pad walk the filtered list
        g_icur = g_icur < 0 ? 0 : g_icur + (down ? 1 : -1); if (g_icur < 0) g_icur = 0; if (g_icur >= g_nidx) g_icur = g_nidx - 1;
        snprintf(g_isel, sizeof g_isel, "%s", cat->it[g_idx[g_icur]].path); g_var = 0; g_iscroll = 1;
    }
    ImGui::TextDisabled("%d of %d items", g_nidx, cat->n);
    ImGui::BeginChild("##list", ImVec2(0.f, -ImGui::GetFrameHeightWithSpacing() * 4.6f), ImGuiChildFlags_Borders);
    ImGuiListClipper clip; clip.Begin(g_nidx); if (g_iscroll && g_icur >= 0) clip.IncludeItemByIndex(g_icur);
    while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
        const CatItem *c = &cat->it[g_idx[r]]; char row[200];
        const char *tail = strrchr(c->path, '/'); tail = tail ? tail + 1 : c->path;
        snprintf(row, sizeof row, "%s   [%s / %s%s]  %.*s%s%s##%d", c->name, cat_kind_text(c->kind), c->slot, c->nvar > 0 ? ", colours" : "", (int)strcspn(tail, "."), tail, c->mod ? "  (mod)" : "", c->owned ? "  - already owned" : "", r);
        if (g_test_select && !g_test_done && !g_isel[0]) { snprintf(g_isel, sizeof g_isel, "%s", c->path); g_test_done = 1; }
        if (ImGui::Selectable(row, !strcmp(g_isel, c->path))) { snprintf(g_isel, sizeof g_isel, "%s", c->path); g_var = 0; g_icur = r; }
        if (g_iscroll && r == g_icur) { ImGui::SetScrollHereY(0.5f); g_iscroll = 0; }
    }
    ImGui::EndChild();

    int si = g_isel[0] ? cat_find(cat, g_isel) : -1;
    CatItem cur; memset(&cur, 0, sizeof cur); if (si >= 0) cur = cat->it[si];
    cheats_catalog_unlock();
    if (si < 0) { ImGui::TextDisabled("Select an item (click, or Up / Down), then Add to inventory (Enter / pad A)."); return; }

    ImGui::Text("%s", cur.name); ImGui::SameLine(); ImGui::TextDisabled("%s / %s%s", cat_kind_text(cur.kind), cur.slot, cur.owned ? " - already owned" : "");
    ImGui::SetNextItemWidth(U(120.f)); ImGui::InputInt("Count", &g_count, 1, 5); g_count = cat_count_clamp(g_count);
    if (cur.nvar > 0) {
        char b[32]; g_var = cat_var_clamp(&cur, g_var); ImGui::SameLine(); ImGui::SetNextItemWidth(U(160.f));
        if (ImGui::BeginCombo("Colour", cat_var_label(&cur, g_var, b, sizeof b))) {
            for (int v = 0; v < cur.nvar; v++) { if (ImGui::Selectable(cat_var_label(&cur, v, b, sizeof b), v == g_var)) g_var = v; }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(); ImGui::Checkbox("Equip after adding", &g_equip);
    if (ImGui::Button("Add to inventory") || use) { A->log(A, "cheats-ui: %s: add %s x%d colour %d equip %d", use ? "key/pad" : "mouse", cur.path, g_count, g_var, g_equip ? 1 : 0); cheats_request_add(cur.path, g_count, cat_var_clamp(&cur, g_var), g_equip ? 1 : 0); }
}

// ---- NPC picker (0.8.0): a button with the chosen NPC; it opens a list with a search box, grouped like F2 > Spawn NPC. The list is the loader's (mods' NPCs
// included), re-read when the tab opens. Left / Right on the row still step through it.
static void npc_picker(int row, int up, int down, int use) {
    char cur[48]; int k = cheats_get_int(1); if (!cheats_npcl_get(k, NULL, 0, cur, sizeof cur)) snprintf(cur, sizeof cur, "(choose)");
    char b[80]; snprintf(b, sizeof b, "%s  v##npcbtn", cur);
    int open = ImGui::Button(b, ImVec2(U(260.f), 0.f)); if (open) g_sel = row;
    if (g_pick_req) { g_pick_req = 0; open = 1; }
    if (open) { ImGui::OpenPopup("##npcpick"); A->log(A, "cheats-ui: NPC picker opened (%d entries)", cheats_npc_count()); }
    ImGui::SetNextWindowSize(ImVec2(U(420.f), U(420.f)), ImGuiCond_Appearing);
    g_pick_open = 0;
    if (ImGui::BeginPopup("##npcpick")) {
        g_pick_open = 1;
        static char q[48]; static int hi = -1;                                  // hi: the highlighted entry (keys / pad), an index into the full list
        int appearing = ImGui::IsWindowAppearing();
        if (appearing) { q[0] = 0; hi = k; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-1.f); ImGui::InputTextWithHint("##q", "type to search: name or group", q, sizeof q);
        ImGui::TextDisabled("Up / Down + Enter (pad: d-pad + A), or click");
        ImGui::BeginChild("##npcl", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders);
        char lq[48]; snprintf(lq, sizeof lq, "%s", q); _strlwr(lq);
        static int vis[512]; int nv = 0, at = -1; char g[40], l[48];
        for (int i = 0; nv < 512 && cheats_npcl_get(i, g, sizeof g, l, sizeof l); i++) {
            if (lq[0]) { char hay[96]; snprintf(hay, sizeof hay, "%s %s", l, g); _strlwr(hay); if (!strstr(hay, lq)) continue; }
            if (i == hi) at = nv; vis[nv++] = i;
        }
        int moved = 0;
        int kup = up || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true), kdn = down || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        if (nv && (kup || kdn)) { at = at < 0 ? 0 : (at + (kdn ? 1 : -1) + nv) % nv; hi = vis[at]; moved = 1; }
        if (nv && at < 0) { at = 0; hi = vis[0]; }
        int pick = -1;
        if (nv && !appearing && (use || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) pick = hi;
        char last[40] = "\x01";
        for (int v = 0; v < nv; v++) {
            int i = vis[v]; cheats_npcl_get(i, g, sizeof g, l, sizeof l);
            if (strcmp(g, last)) { ImGui::SeparatorText(g); snprintf(last, sizeof last, "%s", g); }
            char sl[64]; snprintf(sl, sizeof sl, "%s%s##n%d", l, i == k ? "  (current)" : "", i);
            if (ImGui::Selectable(sl, i == hi)) pick = i;
            if (i == hi && (moved || appearing)) ImGui::SetScrollHereY(0.4f);
        }
        if (!nv) ImGui::TextDisabled("No NPC matches.");
        ImGui::EndChild();
        if (pick >= 0) { cheats_npcl_get(pick, g, sizeof g, l, sizeof l); cheats_set_int(1, pick); A->log(A, "cheats-ui: NPC picker: %d '%s' (%s)", pick, l, g); ImGui::CloseCurrentPopup(); g_pick_open = 0; }
        ImGui::EndPopup();
    }
}

// ---- the window
static void view_rows(int up, int down, int left, int right, int use, const char *src) {
    int pu = 0, pd = 0, pa = 0; if (g_pick_open) { pu = up; pd = down; pa = use; up = down = left = right = use = 0; }   // the open NPC picker has the keys / pad
    if (up || down) { int was = g_sel; g_sel = row_step(g_sel, down ? 1 : -1); g_scroll_sel = 1; if (g_sel != was) A->log(A, "cheats-ui: %s: %s -> row %d '%s'", src, down ? "Down" : "Up", g_sel, row_label(g_sel)); }
    if (left || right) row_adjust(g_sel, right ? 1 : -1, src);
    if (use) row_use(g_sel, src);

    WorldView v; cheats_world_view(&v);
    int np = 0, mp = cheats_mp(&np);
    if (mp) { ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Cheats are OFF: %d players are in this game (co-op / PvP).", np);
              ImGui::TextDisabled("Your toggles are kept and come back when you are alone again."); }
    else if (np > 1 && cheats_mp_allowed()) ImGui::TextColored(ImVec4(1.f, 0.78f, 0.25f, 1.f), "%d players here and cheats stay ON (setting). Every joiner sees \"Host has cheats enabled\".", np);
    else ImGui::TextDisabled("%s", !v.have_pawn ? "No character yet." : v.host ? (cheats_mp_allowed() ? "Cheats keep working when other players join (setting); joiners will see a notice." : "Alone in your game: every row works (cheats switch off when another player joins).") : "Joined client: rows marked (host) only work for the host.");
    if (mp) ImGui::BeginDisabled();
    float foot = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    ImGui::BeginChild("##rows", ImVec2(0.f, -foot), ImGuiChildFlags_Borders);
    const float vx = U(300.f);
    // Toggles are check boxes, actions are buttons, values are number fields / the NPC picker; Up / Down still select a row (yellow
    // frame) and Enter / Space / pad A use it, Left / Right change a value.
    for (int i = 0; i < NROWS; i++) {
        const Row &r = ROWS[i];
        if (r.kind == RK_HEAD) { ImGui::SeparatorText(r.text); continue; }
        ImGui::PushID(i);
        char lab[128]; snprintf(lab, sizeof lab, "%s%s", row_label(i), !v.host && row_host_only(i) ? "  (host)" : "");
        ImVec2 a0 = ImGui::GetCursorScreenPos();
        if (r.kind == RK_TOGGLE) {
            char val[16]; row_value(i, val, sizeof val); bool b = !strcmp(val, "ON");
            if (ImGui::Checkbox(lab, &b)) { g_sel = i; A->log(A, "cheats-ui: mouse: check box row %d '%s'", i, row_label(i)); row_use(i, "mouse"); }
            if (r.id == 4) { ImGui::SameLine(); ImGui::TextDisabled("(%s flies / lands)", cheats_fly_key_name()); }
        } else if (r.kind == RK_VAL) {
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(lab); ImGui::SameLine(vx);
            if (r.id == V_NPC) npc_picker(i, pu, pd, pa);
            else {
                double x = 0, mn = 0, mx = 0, st = 1; int integer = 1;
                switch (r.id) {
                case V_FLY: x = cheats_speed(); mn = 100; mx = 4000; st = 100; break;
                case V_PLAYER: x = cheats_get_int(0); mn = 1; mx = 8; break;
                case V_NPCMAX: x = cheats_get_int(2); mn = 1; mx = 40; break;
                case V_SPEED: x = cheats_dilation(); mn = 0.1; mx = 3.0; st = 0.1; integer = 0; break;
                }
                if (gleam_number("##num", &x, mn, mx, st, integer, U(150.f))) {
                    g_sel = i;
                    switch (r.id) {
                    case V_FLY: cheats_set_speed((float)x); break;
                    case V_PLAYER: cheats_set_int(0, (int)x); break;
                    case V_NPCMAX: cheats_set_int(2, (int)x); break;
                    case V_SPEED: cheats_set_dilation((float)x); break;
                    }
                    char vv[96]; row_value(i, vv, sizeof vv); A->log(A, "cheats-ui: mouse / typed: %s -> %s", row_label(i), vv);
                }
                if (r.id == V_PLAYER) { char vv[96]; row_value(i, vv, sizeof vv); ImGui::SameLine(); ImGui::TextDisabled("%s", vv); }
                if (r.id == V_FLY) { ImGui::SameLine(); ImGui::TextDisabled("uu/s"); }
                if (r.id == V_SPEED) { ImGui::SameLine(); ImGui::TextDisabled("x (Apply below)"); }
            }
        } else {                                                               // RK_ACT / RK_HEAL / RK_ITEMS: a button
            if (ImGui::Button(lab)) { g_sel = i; ImVec2 m = ImGui::GetIO().MousePos; A->log(A, "cheats-ui: mouse: button at %.0f %.0f row %d '%s'", m.x, m.y, i, row_label(i)); row_use(i, "mouse"); }
            char val[96]; row_value(i, val, sizeof val); if (val[0]) { ImGui::SameLine(); ImGui::TextDisabled("%s", val); }
        }
        ImVec2 a1 = ImGui::GetItemRectMax();
        if (i == g_sel) ImGui::GetWindowDrawList()->AddRect(ImVec2(a0.x - 3.f, a0.y - 1.f), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, a1.y + 1.f), IM_COL32(255, 217, 89, 255), 3.f, 0, 1.5f);
        if (!g_layout_logged) { ImVec2 b0 = ImGui::GetItemRectMin(); A->log(A, "cheats-ui: layout row %d '%s' x %.0f-%.0f y %.0f-%.0f (client px)", i, row_label(i), b0.x, a1.x, b0.y, a1.y); }
        if (i == g_sel && g_scroll_sel) { ImGui::SetScrollHereY(0.5f); g_scroll_sel = 0; }
        ImGui::PopID();
    }
    g_layout_logged = 1;
    ImGui::EndChild();
    if (mp) ImGui::EndDisabled();
    char msg[160]; unsigned ms = 0; cheats_status(msg, sizeof msg, &ms); ImGui::TextWrapped("%s", msg[0] ? msg : " ");
}

static void draw(void *) {
    if (!g_open) return;                                                    // the fix: drawn only while open, whoever keeps the overlay running
    if (!bind()) { cui_set_open(0); return; }                               // never hold the game's input with nothing on screen
    ImGuiIO &io = ImGui::GetIO();
    g_sc = io.FontGlobalScale > 0.5f ? io.FontGlobalScale : 1.f;
    static int tinit; if (!tinit) { tinit = 1; char q[96]; g_test_tab = cheats_test_ui(q, sizeof q, &g_test_select); if (q[0]) snprintf(g_query, sizeof g_query, "%s", q); if (g_test_tab) g_view_items = 1; }
    static LONG seen_open; int fresh = seen_open != g_opened_by; seen_open = g_opened_by;
    if (fresh) g_pad_sync = 1;

    // One input owner (loader API 2.23 input_owner): keys, pad and Esc act here only while this window has the focus (or nobody's window has it);
    // with the loader's F1 / F2 menu or another mod's window focused they are left to that one (click here or press the window key to take them back).
    // Older loaders: the 0.6.0 rule (a loader menu drawn this frame takes the keys; the pad stays here).
    int owner = A->size >= offsetof(AbsApi, input_owner) + sizeof(void *) && A->input_owner ? A->input_owner(A) : -1;
    ImGuiWindow *lw = ImGui::FindWindowByName("###lnwin"); int menu = owner >= 0 ? (owner == 1 || owner == 2) : (lw && lw->Active);
    static int menu_was = -1; if (menu != menu_was) { menu_was = menu; A->log(A, "cheats-ui: input owner %d - %s", owner, menu ? "keys and pad left to the loader menu / another window" : "keys and pad here"); }
    int keys = !menu && !io.WantTextInput;
    int pad[P_N]; pad_poll(pad);
    if (menu && owner >= 0) for (int k = 0; k < P_N; k++) pad[k] = 0;
    int up = (keys && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) || pad[P_UP], down = (keys && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) || pad[P_DOWN];
    int left = (keys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) || pad[P_LEFT], right = (keys && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) || pad[P_RIGHT];
    int use = (keys && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))) || pad[P_A];
    int back = ((owner < 0 || !menu) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) || pad[P_B];
    const char *src = pad[P_UP] || pad[P_DOWN] || pad[P_LEFT] || pad[P_RIGHT] || pad[P_A] || pad[P_B] ? "pad" : "key";
    if (back) {
        if (g_view_items) { g_view_items = 0; A->log(A, "cheats-ui: %s: Esc / B -> rows", src); }
        else { A->log(A, "cheats-ui: %s: Esc / B -> close", src); cui_set_open(0); return; }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) A->log(A, "cheats-ui: mouse: left click at %.0f %.0f", io.MousePos.x, io.MousePos.y);

    float h = io.DisplaySize.y - U(120.f); if (h > U(860.f)) h = U(860.f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - U(580.f), U(80.f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(U(540.f), h), ImGuiCond_FirstUseEver);
    if (fresh) ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowBgAlpha(0.94f);
    char title[96]; snprintf(title, sizeof title, "Cheats  (%s / Esc closes)###cheatsmod", cheats_ui_key_name());
    bool open = true;
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        if (g_view_items) view_items(up, down, use);
        else view_rows(up, down, left, right, use, src);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", menu ? (owner >= 0 ? "Another window has the keys and the pad: click here to use them in this window." : "Close the F1 / F2 menu to use the keys here; the mouse and pad work.") :
                                        "Up / Down select, Left / Right change, Enter / Space / pad A use, Esc / pad B back or close.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    if (!open) { A->log(A, "cheats-ui: mouse: X -> close"); cui_set_open(0); }
}

// the loader tab (API 2.33): drawn inside the loader's window while the tab shows - no Begin / End, no input mode of its own (the open loader menu keeps
// the game's keys, mouse and pad). input_owner() is 3 while the tab shows: Up / Down / Left / Right / Enter / Space and the pad's d-pad / A are read here;
// Esc and pad B close the loader window (the loader reads them), Backspace / the Back button leave the items page.
static void tab_draw(void *) {
    if (!bind()) return;
    ImGuiIO &io = ImGui::GetIO();
    g_sc = io.FontGlobalScale > 0.5f ? io.FontGlobalScale : 1.f;
    static int tinit; if (!tinit) { tinit = 1; char q[96]; g_test_tab = cheats_test_ui(q, sizeof q, &g_test_select); if (q[0]) snprintf(g_query, sizeof g_query, "%s", q); if (g_test_tab) g_view_items = 1; }
    ULONGLONG now = GetTickCount64(); int fresh = now - g_tab_seen > 300; g_tab_seen = now;
    if (fresh) { g_pad_sync = 1; cheats_npcl_refresh(); A->log(A, "cheats-ui: tab shown (NPC list re-read)"); }
    int owner = A->input_owner(A), keys = owner == 3 && !io.WantTextInput;
    int pad[P_N]; pad_poll(pad); if (owner != 3) for (int k = 0; k < P_N; k++) pad[k] = 0;
    int up = (keys && ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) || pad[P_UP], down = (keys && ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) || pad[P_DOWN];
    int left = (keys && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) || pad[P_LEFT], right = (keys && ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) || pad[P_RIGHT];
    int use = (keys && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))) || pad[P_A];
    const char *src = pad[P_UP] || pad[P_DOWN] || pad[P_LEFT] || pad[P_RIGHT] || pad[P_A] ? "pad" : "key";
    if (g_view_items && keys && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) { g_view_items = 0; A->log(A, "cheats-ui: key: Backspace -> rows"); }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) A->log(A, "cheats-ui: mouse: left click at %.0f %.0f", io.MousePos.x, io.MousePos.y);
    float foot = ImGui::GetTextLineHeightWithSpacing() * 1.2f;
    ImGui::BeginChild("##cheatstab", ImVec2(0.f, -foot));
    if (g_view_items) view_items(up, down, use); else view_rows(up, down, left, right, use, src);
    ImGui::EndChild();
    ImGui::TextDisabled("Up / Down select, Left / Right change, Enter / Space / pad A use%s.  %s: this tab", g_view_items ? ", Backspace back" : "", cheats_ui_key_name());
}
extern "C" int cui_start(const AbsApi *api) {
    A = api;
    if (api->size < offsetof(AbsApi, overlay_raw_mouse) + sizeof(void *)) return 2;
    if (api->size >= offsetof(AbsApi, ui_tab_show) + sizeof(void *) && api->ui_tab) {
        int rc = api->ui_tab(api, "Cheats", tab_draw, NULL);
        api->log(api, "cheats-ui: ui_tab -> %d (%s)", rc, rc ? "falling back to the own window" : "the rows are a tab of the loader's Mods window");
        if (!rc) { g_tab = 1; return 0; }
    }
    return api->overlay_register(api, draw, NULL);
}
extern "C" void cui_set_open(int open) {
    if (!A) return;
    if (g_tab) { int rc = A->ui_tab_show(A, open ? 1 : 0); A->log(A, "cheats-ui: tab %s (ui_tab_show -> %d)", open ? "OPEN" : "closed", rc); return; }   // game thread (window key, F2 row)
    LONG was = InterlockedExchange(&g_open, open ? 1 : 0);
    if (open && !was) InterlockedIncrement(&g_opened_by);
    int rc = A->overlay_set_mode(A, open ? (ABS_OV_DRAW | ABS_OV_MOUSE | ABS_OV_KEYS | ABS_OV_BLOCK_GAME) : 0);
    if (!!was != !!open) A->log(A, "cheats-ui: window %s (overlay_set_mode -> %d)", open ? "OPEN" : "closed", rc);
}
extern "C" int cui_is_open(void) { return g_tab ? GetTickCount64() - g_tab_seen < 300 : g_open != 0; }
