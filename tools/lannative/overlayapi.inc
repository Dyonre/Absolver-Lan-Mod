// overlayapi.inc - AbsApi::overlay_info / overlay_register / overlay_unregister / overlay_set_mode / overlay_raw_mouse (included by content.inc; needs LN_OVERLAY).
// Plugins draw Dear ImGui windows through the loader's single overlay (overlay.cpp). Draw callbacks run on the render thread inside a guard; the guard's fault counter is the
// plugin-wide one (3 faults switch every callback of the plugin off, this one included), and a plugin that is switched off or unloaded loses its overlay modes and callbacks
// (ov_release_owner). The overlay is installed at the first registration or mode request, never at start-up. The loader's own menu style (menu_style) does not affect plugins.
#ifdef LN_OVERLAY
static int ovapi_alive(int p) { return plug_guard(p); }
static void ovapi_fault(int p, unsigned long code) { plug_fault(p, "overlay draw callback", code); }
static const char *ovapi_name(int p) { return plug_name_of(p); }
static int ovapi_up(int p, const char *what) {                                  // 0 = usable, 2 = unavailable
    ov_set_log(logf_); ov_set_guard(ovapi_alive, ovapi_fault, ovapi_name);
    if (ov_state() == OV_OFF) ov_request();
    if (ov_state() == OV_DEAD) { logf_("plugin %s: %s refused - the overlay is not available", plug_name_of(p), what); return 2; }
    return 0;
}
static int api_p_overlay_info(const AbsApi *self, AbsOverlayInfo *out) {
    if (plug_of(self) < 0 || !out || out->size < offsetof(AbsOverlayInfo, width) + sizeof(int)) return 1;
    if (ov_state() == OV_OFF) return 1;
    OvInfo in; ov_info(plug_of(self), &in);
    AbsOverlayInfo o; memset(&o, 0, sizeof o); o.state = in.state; o.imgui_context = in.context; o.imgui_version = in.version; o.imgui_version_num = in.version_num;
    o.imgui_alloc = in.alloc; o.imgui_free = in.free; o.imgui_alloc_user = in.alloc_user; o.width = in.width; o.height = in.height; o.ui_scale = in.scale; o.d3d_device = in.device; o.d3d_context = in.device_context;
    o.caller_mode = in.caller_mode; o.caller_callbacks = in.caller_callbacks;
    o.sizeof_imgui_io = in.sizeof_io; o.sizeof_imgui_style = in.sizeof_style; o.sizeof_imgui_drawvert = in.sizeof_vert; o.sizeof_imgui_drawidx = in.sizeof_idx; o.sizeof_imgui_wchar = in.sizeof_wchar;
    uint32_t cap = out->size < sizeof o ? out->size : (uint32_t)sizeof o; memcpy(out, &o, cap); out->size = cap;
    return in.state < 0 ? 2 : in.state == 0 ? 1 : 0;
}
static int api_p_overlay_register(const AbsApi *self, void (*draw)(void *user), void *user) {
    int p = plug_of(self); if (p < 0 || p > 63 || !draw) return 1;
    int rc = ovapi_up(p, "overlay_register"); if (rc) return rc;
    rc = ov_register(p, draw, user); logf_("plugin %s: overlay_register -> %d", plug_name_of(p), rc); return rc;
}
static int api_p_overlay_unregister(const AbsApi *self, void (*draw)(void *user), void *user) {
    int p = plug_of(self); if (p < 0 || p > 63 || !draw || ov_state() == OV_OFF) return 1;
    int rc = ov_unregister(p, draw, user); logf_("plugin %s: overlay_unregister -> %d", plug_name_of(p), rc); return rc;
}
static int api_p_overlay_set_mode(const AbsApi *self, unsigned flags) {
    int p = plug_of(self); if (p < 0 || p > 63) return 1;
    if (!flags && ov_state() == OV_OFF) return 0;                                // nothing to switch off yet
    int rc = ovapi_up(p, "overlay_set_mode"); if (rc) return rc;
    return ov_set_mode(p, flags);
}
static int api_p_overlay_raw_mouse(const AbsApi *self, int *dx, int *dy) {
    if (plug_of(self) < 0 || !dx || !dy || ov_state() == OV_OFF || ov_state() == OV_DEAD) return 1;
    return ov_raw_mouse(dx, dy);
}
static void ov_fill_api(AbsApi *a) {
    a->overlay_info = api_p_overlay_info; a->overlay_register = api_p_overlay_register; a->overlay_unregister = api_p_overlay_unregister;
    a->overlay_set_mode = api_p_overlay_set_mode; a->overlay_raw_mouse = api_p_overlay_raw_mouse;
}
#else
static void ov_fill_api(AbsApi *a) { (void)a; }
#endif
