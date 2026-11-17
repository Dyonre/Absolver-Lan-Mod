// logwin.inc - F2 row that shows / hides the log window (window: overlay.cpp draw_log, data: logview.h).
// The window lists the last 3000 lines of LanNative\lannative.log with a filter box (text or mod name), "Errors only" and each line's mod tag; read-only.
// It is drawn beside the loader's window while F1 / F2 is open (overlay menu style only).
#ifdef LN_OVERLAY
static char g_lw_row[128];
static int lw_rows_count(void) {
    int up = ov_state() == OV_READY || ov_state() == OV_STARTING;
    snprintf(g_lw_row, sizeof g_lw_row, "Log window: %s  (lannative.log: filter, errors only, mod tags)", !up ? "needs the overlay (menu_style = imgui)" : ov_log_window_shown() ? "shown - Enter / click: hide" : "Enter / click: show");
    return 1;
}
static const char *lw_rows_label(int i) { return i == 0 ? g_lw_row : ""; }
static void lw_rows_action(int i) {
    if (i != 0) return;
    if (ov_state() != OV_READY && ov_state() != OV_STARTING) { logf_("mods page: log window refused - the overlay is not running (menu_style = text?)"); return; }
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%slannative.log", g_dir); ov_log_window(p, !ov_log_window_shown());
}
LN_MODS_ROWS(0050, logwin, lw_rows_count, lw_rows_label, lw_rows_action)   // first of the units' rows: right after the plugin sections
#endif
