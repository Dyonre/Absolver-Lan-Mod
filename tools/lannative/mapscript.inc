// mapscript.inc - generic map script of the script extender (included by secore.inc after modes.inc). Guide: docs\MODDING-GUIDE.md "Map script".
// A mod ships  Mods\<mod>\content\maps\*.json  (content kind "maps", registered by content_init). One entry = one scene for the maps whose world path contains `map`:
//   lights (point / spot, colour + intensity from key-frame curves, on/off by phase), meshes (movable props / doors with named states, collision switched with the state),
//   triggers (player in a box -> set a mesh state, a phase, a light, a banner message) and curves. Everything is spawned at runtime with engine classes (APointLight, ASpotLight,
//   AStaticMeshActor) driven by UFunction name; the one native call is USceneComponent::SetMobility (PDB symbol). Nothing is replicated; every machine runs its own copy.
// Pure logic (curves, phases, placement, trigger edges, timed moves): mapscript.h. Plugin hooks: AbsApi::mapscript_phase / mapscript_mesh.
#include "mapscript.h"
#define MS_LIGHTS 64                // per scene (parse limit); what is spawned is capped by MS_MAX_LIGHTS_TOTAL over all scenes
#define MS_MESHES 96                // per scene, grid cells included
#define MS_USER_MESHES 16           // entries of "meshes"
#define MS_TRIGS 8
#define MS_CURVES 12
#define MS_EVENTS 8
#define MS_SCENES 4
#define MS_ACTS 6
#define MS_MAX_LIGHTS_TOTAL 48
#define MS_NAV_RADIUS_MARGIN 5.f       // uu added to the player's capsule radius for the navmesh agent (keeps paths off wall corners); tune if bots still rub corners
typedef struct { int kind; char target[32], state[16]; int on; float secs; char text[100]; } MsAct;         // kind 0 mesh state, 1 phase, 2 light on/off, 3 banner message, 4 phase cycle (text = "a,b,c")
typedef struct {
    char id[32], type; float pos[3], pitch, yaw, color[3], intensity, radius, cone_inner, cone_outer, phase_offset; int shadows; char color_curve[32], intensity_curve[32], on[40];
    void *actor, *comp; int vis; float cur_c[3], cur_i; int forced;                       // runtime; forced: 0 follow `on`, 1 on, 2 off (set by a light action)
} MsLight;
typedef struct { char name[16]; float off[3]; int collision; } MsState;
typedef struct {
    char id[32], mesh[200], material[200], param[32], color_curve[32], on[40]; float pos[3], yaw, scale[3], move_time, color[3], brightness, phase_offset; int nst, cur, initial, pending_col, tinted, grid; MsState st[4];
    void *actor, *comp, *mid; float base[3], delta[3], cur_c[3]; int hidden; uint64_t pname; MsMove mv;   // runtime
} MsMesh;
typedef struct { char id[32]; float center[3], half[3]; int once, fired, inside, nenter, nexit; MsAct enter[MS_ACTS], exit[MS_ACTS]; } MsTrig;
typedef struct { int kind; float arg; char text[40], cond[40]; int nd, nu; MsAct d[MS_ACTS], u[MS_ACTS]; int state; float acc; } MsEvent;   // kind 0 start, 1 score >= arg, 2 match state contains text, 3 every arg seconds
typedef struct {
    char id[CR_ID_MAX], mod[MOD_NAME_MAX], map[100]; int relative, server_clock, nl, nm, nt, nc, ne, ngrid; float offset[3], delay, cal[3];   // cal: calibration { gamma, intensity_scale, saturation }
    MsCurve c[MS_CURVES]; MsLight l[MS_LIGHTS]; MsMesh m[MS_MESHES]; MsTrig t[MS_TRIGS]; MsEvent ev[MS_EVENTS];
    int live; float origin[3], yaw;                                                            // runtime: spawned in the current world
    int nav; float nav_c[3], nav_h[3];                                                         // "navmesh": a runtime RecastNavMesh over this box (world coordinates)
} MsScene;
static MsScene *g_ms[MS_SCENES]; static int g_ms_n;                                            // parsed scenes (heap)
static MsPhases g_ms_ph; static unsigned g_ms_world; static float g_ms_wait, g_ms_clock; static int g_ms_lights_live, g_ms_rebuild; static unsigned g_ms_tick_n;
static char g_ms_world_path[300];

// ----- JSON helpers
static double ms_num(const JDoc *d, int n) { return n >= 0 && d->n[n].str ? atof(d->n[n].str) : 0; }
static const char *ms_str(const JDoc *d, int o, const char *k) { int c = jfind(d, o, k); return c >= 0 && d->n[c].t == J_STR ? d->n[c].str : NULL; }
static double ms_numk(const JDoc *d, int o, const char *k, double dflt) { int c = jfind(d, o, k); return c >= 0 && d->n[c].t == J_NUM ? ms_num(d, c) : dflt; }
static int ms_boolk(const JDoc *d, int o, const char *k, int dflt) { int c = jfind(d, o, k); if (c < 0) return dflt; if (d->n[c].t == J_BOOL) return d->n[c].str && !strcmp(d->n[c].str, "True"); if (d->n[c].t == J_NUM) return ms_num(d, c) != 0; return dflt; }
static int ms_vec(const JDoc *d, int o, const char *k, float *out, int n, const float *dflt) {    // array of n numbers; missing key = default (1), wrong shape = error (0)
    int c = jfind(d, o, k); for (int i = 0; i < n; i++) out[i] = dflt ? dflt[i] : 0; if (c < 0) return 1; if (d->n[c].t != J_ARR) return 0;
    int i = 0; for (int e = d->n[c].first; e >= 0; e = d->n[e].next) { if (d->n[e].t != J_NUM || i >= n) return 0; out[i++] = (float)ms_num(d, e); } return i == n;
}
static int ms_id_ok(const char *s) { if (!s || !*s || strlen(s) > 31) return 0; for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '_')) return 0; return 1; }
static int ms_curve_find(const MsScene *s, const char *id, int dims) { if (!id || !*id) return -1; for (int i = 0; i < s->nc; i++) if (!strcmp(s->c[i].id, id) && s->c[i].dims == dims) return i; return -2; }
static int ms_parse_acts(const JDoc *d, int arr, MsAct *out, int *n, char *err, int cap) {
    *n = 0; if (arr < 0) return 1; if (d->n[arr].t != J_ARR) { snprintf(err, (size_t)cap, "actions must be an array"); return 0; }
    for (int e = d->n[arr].first; e >= 0; e = d->n[e].next) {
        if (*n >= MS_ACTS) { snprintf(err, (size_t)cap, "at most %d actions per trigger side", MS_ACTS); return 0; } MsAct *a = &out[*n]; memset(a, 0, sizeof *a); const char *v;
        if ((v = ms_str(d, e, "mesh"))) { a->kind = 0; snprintf(a->target, sizeof a->target, "%s", v); v = ms_str(d, e, "state"); if (!v) { snprintf(err, (size_t)cap, "mesh action needs a \"state\""); return 0; } snprintf(a->state, sizeof a->state, "%s", v); }
        else if ((v = ms_str(d, e, "phase"))) { a->kind = 1; snprintf(a->target, sizeof a->target, "%s", v); a->on = ms_boolk(d, e, "on", 1); }
        else if ((v = ms_str(d, e, "light"))) { a->kind = 2; snprintf(a->target, sizeof a->target, "%s", v); a->on = ms_boolk(d, e, "on", 1); }
        else if ((v = ms_str(d, e, "message"))) { a->kind = 3; snprintf(a->text, sizeof a->text, "%s", v); a->secs = (float)ms_numk(d, e, "seconds", 3.0); }
        else if (jfind(d, e, "cycle") >= 0) {                                   // {"cycle": ["show_a","show_b"]}: next phase of the list on, the others off
            int c = jfind(d, e, "cycle"), k = 0; a->kind = 4; size_t l = 0;
            if (d->n[c].t != J_ARR) { snprintf(err, (size_t)cap, "\"cycle\" must be an array of phase names"); return 0; }
            for (int q = d->n[c].first; q >= 0; q = d->n[q].next, k++) { if (d->n[q].t != J_STR || !d->n[q].str || !*d->n[q].str || strchr(d->n[q].str, ',') || strlen(d->n[q].str) > 23 || k >= 8 || l + strlen(d->n[q].str) + 2 > sizeof a->text) { snprintf(err, (size_t)cap, "\"cycle\": 1..8 phase names of at most 23 characters"); return 0; } l += (size_t)snprintf(a->text + l, sizeof a->text - l, "%s%s", k ? "," : "", d->n[q].str); }
            if (!k) { snprintf(err, (size_t)cap, "\"cycle\" is empty"); return 0; }
        }
        else { snprintf(err, (size_t)cap, "an action needs one of mesh / phase / light / message / cycle"); return 0; }
        (*n)++;
    }
    return 1;
}
// look of a mesh / grid: material, colour (MID vector parameter), colour curve, brightness, on-condition
static int ms_parse_look(const JDoc *d, int e, const MsScene *s, MsMesh *m, char *err, int cap) {
    const char *v = ms_str(d, e, "material"); snprintf(m->material, sizeof m->material, "%s", v ? v : "");
    if (m->material[0]) { char why[200] = ""; if (!rows_objref_ok("Object", m->material, why, sizeof why)) { snprintf(err, (size_t)cap, "%s: material: %s", m->id, why); return 0; } }
    v = ms_str(d, e, "param"); snprintf(m->param, sizeof m->param, "%s", v ? v : "Color");
    int hc = jfind(d, e, "color") >= 0; if (!ms_vec(d, e, "color", m->color, 3, NULL)) { snprintf(err, (size_t)cap, "%s: color is [r,g,b]", m->id); return 0; }
    m->brightness = (float)ms_numk(d, e, "brightness", 1); m->phase_offset = (float)ms_numk(d, e, "phase_offset", 0);
    if (!(m->brightness >= 0 && m->brightness <= 100)) { snprintf(err, (size_t)cap, "%s: brightness 0..100", m->id); return 0; }
    v = ms_str(d, e, "color_curve"); if (v) { if (ms_curve_find(s, v, 3) < 0) { snprintf(err, (size_t)cap, "%s: colour curve '%s' not found", m->id, v); return 0; } snprintf(m->color_curve, sizeof m->color_curve, "%s", v); }
    m->tinted = hc || m->color_curve[0];
    v = ms_str(d, e, "on"); snprintf(m->on, sizeof m->on, "%s", v ? v : "always");
    return 1;
}
// text -> scene; 0 with a reason in err
static int ms_parse(const char *json, MsScene *s, char *err, int cap) {
    JDoc d; if (jdoc_parse(&d, json) != 0) { snprintf(err, (size_t)cap, "JSON: %s", d.err); jdoc_free(&d); return 0; }
    int ok = 0, o = 0; memset(s, 0, sizeof *s); err[0] = 0; const char *v;
    if (d.n[0].t != J_OBJ) { snprintf(err, (size_t)cap, "an entry must be an object"); goto done; }
    v = ms_str(&d, o, "id"); if (!v || strlen(v) >= sizeof s->id) { snprintf(err, (size_t)cap, "bad id"); goto done; } snprintf(s->id, sizeof s->id, "%s", v);
    v = ms_str(&d, o, "map"); if (!v || !*v || strlen(v) >= sizeof s->map) { snprintf(err, (size_t)cap, "\"map\" (part of the world path, for example MainMap_Main, or \"*\") is required"); goto done; } snprintf(s->map, sizeof s->map, "%s", v);
    v = ms_str(&d, o, "origin"); s->relative = v && !strcmp(v, "player");
    if (v && strcmp(v, "player") && strcmp(v, "world")) { snprintf(err, (size_t)cap, "origin must be \"world\" or \"player\""); goto done; }
    v = ms_str(&d, o, "clock"); s->server_clock = v && !strcmp(v, "server");
    if (!ms_vec(&d, o, "offset", s->offset, 3, NULL)) { snprintf(err, (size_t)cap, "\"offset\" must be [x,y,z]"); goto done; }
    s->delay = (float)ms_numk(&d, o, "delay", 2.0); if (s->delay < 0 || s->delay > 60) { snprintf(err, (size_t)cap, "delay must be 0..60 seconds"); goto done; }
    ms_cal_default(s->cal);                                                                     // "calibration": { "gamma", "intensity_scale", "saturation" } scales every light / mesh colour of this scene (default identity)
    { int cb = jfind(&d, o, "calibration"); if (cb >= 0) {
        if (d.n[cb].t != J_OBJ) { snprintf(err, (size_t)cap, "\"calibration\" must be an object {gamma, intensity_scale, saturation}"); goto done; }
        s->cal[0] = (float)ms_numk(&d, cb, "gamma", 1); s->cal[1] = (float)ms_numk(&d, cb, "intensity_scale", 1); s->cal[2] = (float)ms_numk(&d, cb, "saturation", 1);
        if (!ms_cal_ok(s->cal)) { snprintf(err, (size_t)cap, "calibration: gamma 0.2..5, intensity_scale 0..10, saturation 0..2"); goto done; } } }
    { int nb = jfind(&d, o, "navmesh"); if (nb >= 0) {                                         // "navmesh": {"center": [x,y,z], "half": [x,y,z]} (world coordinates, also with origin "player")
        if (d.n[nb].t != J_OBJ || jfind(&d, nb, "center") < 0 || jfind(&d, nb, "half") < 0 || !ms_vec(&d, nb, "center", s->nav_c, 3, NULL) || !ms_vec(&d, nb, "half", s->nav_h, 3, NULL) || !ms_nav_half_ok(s->nav_h)) {
            snprintf(err, (size_t)cap, "\"navmesh\" must be {\"center\": [x,y,z], \"half\": [x,y,z]} with every half-size 100..50000"); goto done; }
        s->nav = 1; } }
    int cs = jfind(&d, o, "curves");                                                            // curves: { name: {type, loop, keys:[[t, v | [r,g,b]], ...]} }
    if (cs >= 0) {
        if (d.n[cs].t != J_OBJ) { snprintf(err, (size_t)cap, "\"curves\" must be an object"); goto done; }
        for (int e = d.n[cs].first; e >= 0; e = d.n[e].next) {
            if (s->nc >= MS_CURVES) { snprintf(err, (size_t)cap, "at most %d curves", MS_CURVES); goto done; } MsCurve *c = &s->c[s->nc]; if (!d.n[e].key || strlen(d.n[e].key) >= sizeof c->id) { snprintf(err, (size_t)cap, "bad curve name"); goto done; }
            snprintf(c->id, sizeof c->id, "%s", d.n[e].key); v = ms_str(&d, e, "type"); c->dims = v && !strcmp(v, "float") ? 1 : 3; if (!v || (strcmp(v, "float") && strcmp(v, "color"))) { snprintf(err, (size_t)cap, "curve %s: type must be \"color\" or \"float\"", c->id); goto done; }
            c->loop = (float)ms_numk(&d, e, "loop", 0); int ks = jfind(&d, e, "keys");
            if (ks < 0 || d.n[ks].t != J_ARR) { snprintf(err, (size_t)cap, "curve %s: \"keys\" array missing", c->id); goto done; }
            for (int k = d.n[ks].first; k >= 0; k = d.n[k].next) {
                int tn = d.n[k].first, vn = tn >= 0 ? d.n[tn].next : -1; float val[3] = { 0, 0, 0 };
                if (d.n[k].t != J_ARR || tn < 0 || vn < 0 || d.n[tn].t != J_NUM) { snprintf(err, (size_t)cap, "curve %s: a key is [time, value]", c->id); goto done; }
                if (c->dims == 1) { if (d.n[vn].t != J_NUM) { snprintf(err, (size_t)cap, "curve %s: float key value must be a number", c->id); goto done; } val[0] = (float)ms_num(&d, vn); }
                else { int i = 0; if (d.n[vn].t != J_ARR) { snprintf(err, (size_t)cap, "curve %s: colour key value must be [r,g,b]", c->id); goto done; } for (int q = d.n[vn].first; q >= 0 && i < 3; q = d.n[q].next) val[i++] = (float)ms_num(&d, q); if (i < 3) { snprintf(err, (size_t)cap, "curve %s: colour key value must be [r,g,b]", c->id); goto done; } }
                if (!ms_curve_add(c, (float)ms_num(&d, tn), val)) { snprintf(err, (size_t)cap, "curve %s: keys must have ascending times, at most %d keys", c->id, MS_KEYS); goto done; }
            }
            if (!c->n) { snprintf(err, (size_t)cap, "curve %s has no keys", c->id); goto done; } s->nc++;
        }
    }
    int ls = jfind(&d, o, "lights");
    if (ls >= 0) {
        if (d.n[ls].t != J_ARR) { snprintf(err, (size_t)cap, "\"lights\" must be an array"); goto done; }
        for (int e = d.n[ls].first; e >= 0; e = d.n[e].next) {
            if (s->nl >= MS_LIGHTS) { snprintf(err, (size_t)cap, "at most %d lights per scene", MS_LIGHTS); goto done; } MsLight *l = &s->l[s->nl];
            v = ms_str(&d, e, "id"); if (!ms_id_ok(v)) { snprintf(err, (size_t)cap, "light %d: bad or missing id", s->nl + 1); goto done; } snprintf(l->id, sizeof l->id, "%s", v);
            v = ms_str(&d, e, "type"); l->type = v && !strcmp(v, "spot") ? 's' : 'p'; if (v && strcmp(v, "spot") && strcmp(v, "point")) { snprintf(err, (size_t)cap, "light %s: type must be point or spot", l->id); goto done; }
            static const float white[3] = { 1, 1, 1 }; float rot[3] = { 0, 0, 0 }, cone[2] = { 20, 45 };
            if (!ms_vec(&d, e, "pos", l->pos, 3, NULL) || !ms_vec(&d, e, "color", l->color, 3, white) || !ms_vec(&d, e, "rot", rot, 3, NULL) || !ms_vec(&d, e, "cone", cone, 2, cone)) { snprintf(err, (size_t)cap, "light %s: pos / color / rot are [x,y,z] and cone is [inner,outer]", l->id); goto done; }
            l->pitch = rot[0]; l->yaw = rot[1]; l->cone_inner = cone[0]; l->cone_outer = cone[1]; l->intensity = (float)ms_numk(&d, e, "intensity", 5000); l->radius = (float)ms_numk(&d, e, "radius", 500); l->phase_offset = (float)ms_numk(&d, e, "phase_offset", 0); l->shadows = ms_boolk(&d, e, "shadows", 0);
            if (!(l->intensity >= 0 && l->intensity <= 2000000) || !(l->radius > 0 && l->radius <= 10000)) { snprintf(err, (size_t)cap, "light %s: intensity 0..2000000, radius 1..10000", l->id); goto done; }
            for (int i = 0; i < 3; i++) if (!(l->color[i] >= 0 && l->color[i] <= 1)) { snprintf(err, (size_t)cap, "light %s: colour components are 0..1", l->id); goto done; }
            v = ms_str(&d, e, "on"); snprintf(l->on, sizeof l->on, "%s", v ? v : "always");
            v = ms_str(&d, e, "color_curve"); if (v) { if (ms_curve_find(s, v, 3) < 0) { snprintf(err, (size_t)cap, "light %s: colour curve '%s' not found (declare it in \"curves\" before the lights)", l->id, v); goto done; } snprintf(l->color_curve, sizeof l->color_curve, "%s", v); }
            v = ms_str(&d, e, "intensity_curve"); if (v) { if (ms_curve_find(s, v, 1) < 0) { snprintf(err, (size_t)cap, "light %s: float curve '%s' not found", l->id, v); goto done; } snprintf(l->intensity_curve, sizeof l->intensity_curve, "%s", v); }
            s->nl++;
        }
    }
    int ms_ = jfind(&d, o, "meshes");
    if (ms_ >= 0) {
        if (d.n[ms_].t != J_ARR) { snprintf(err, (size_t)cap, "\"meshes\" must be an array"); goto done; }
        for (int e = d.n[ms_].first; e >= 0; e = d.n[e].next) {
            if (s->nm >= MS_USER_MESHES) { snprintf(err, (size_t)cap, "at most %d meshes per scene", MS_USER_MESHES); goto done; } MsMesh *m = &s->m[s->nm];
            v = ms_str(&d, e, "id"); if (!ms_id_ok(v)) { snprintf(err, (size_t)cap, "mesh %d: bad or missing id", s->nm + 1); goto done; } snprintf(m->id, sizeof m->id, "%s", v);
            v = ms_str(&d, e, "mesh"); snprintf(m->mesh, sizeof m->mesh, "%s", v && *v ? v : "/Engine/BasicShapes/Cube.Cube");
            { char why[200] = ""; if (!rows_objref_ok("Object", m->mesh, why, sizeof why)) { snprintf(err, (size_t)cap, "mesh %s: %s", m->id, why); goto done; } }
            static const float one[3] = { 1, 1, 1 };
            if (!ms_vec(&d, e, "pos", m->pos, 3, NULL) || !ms_vec(&d, e, "scale", m->scale, 3, one)) { snprintf(err, (size_t)cap, "mesh %s: pos / scale are [x,y,z]", m->id); goto done; }
            m->yaw = (float)ms_numk(&d, e, "yaw", 0); m->move_time = (float)ms_numk(&d, e, "move_time", 0.8); if (m->move_time < 0 || m->move_time > 30) { snprintf(err, (size_t)cap, "mesh %s: move_time 0..30", m->id); goto done; }
            int sts = jfind(&d, e, "states");
            if (sts >= 0) {
                if (d.n[sts].t != J_OBJ) { snprintf(err, (size_t)cap, "mesh %s: \"states\" must be an object", m->id); goto done; }
                for (int q = d.n[sts].first; q >= 0; q = d.n[q].next) {
                    if (m->nst >= 4) { snprintf(err, (size_t)cap, "mesh %s: at most 4 states", m->id); goto done; } MsState *st = &m->st[m->nst]; if (!d.n[q].key || strlen(d.n[q].key) >= sizeof st->name) { snprintf(err, (size_t)cap, "mesh %s: bad state name", m->id); goto done; }
                    snprintf(st->name, sizeof st->name, "%s", d.n[q].key); if (!ms_vec(&d, q, "offset", st->off, 3, NULL)) { snprintf(err, (size_t)cap, "mesh %s state %s: offset is [x,y,z]", m->id, st->name); goto done; } st->collision = ms_boolk(&d, q, "collision", 1); m->nst++;
                }
            }
            if (!m->nst) { snprintf(m->st[0].name, sizeof m->st[0].name, "default"); m->st[0].collision = 1; m->nst = 1; }
            v = ms_str(&d, e, "initial"); if (v) { m->initial = -1; for (int i = 0; i < m->nst; i++) if (!strcmp(m->st[i].name, v)) m->initial = i; if (m->initial < 0) { snprintf(err, (size_t)cap, "mesh %s: initial state '%s' not in states", m->id, v); goto done; } }
            if (!ms_parse_look(&d, e, s, m, err, cap)) goto done;
            s->nm++;
        }
    }
    int gs_ = jfind(&d, o, "grids");                                                            // grids: one mesh per cell, colour from a curve with a per-cell time offset
    if (gs_ >= 0) {
        if (d.n[gs_].t != J_ARR) { snprintf(err, (size_t)cap, "\"grids\" must be an array"); goto done; }
        for (int e = d.n[gs_].first; e >= 0; e = d.n[e].next) {
            MsMesh g; memset(&g, 0, sizeof g); float org[3], cell[2] = { 100, 100 }, cnt[2] = { 0, 0 }; static const float one[3] = { 1, 1, 1 };
            v = ms_str(&d, e, "id"); if (!ms_id_ok(v) || strlen(v) > 24) { snprintf(err, (size_t)cap, "grid %d: bad or missing id (at most 24 characters)", s->ngrid + 1); goto done; } snprintf(g.id, sizeof g.id, "%s", v);
            v = ms_str(&d, e, "mesh"); snprintf(g.mesh, sizeof g.mesh, "%s", v && *v ? v : "/Engine/BasicShapes/Plane.Plane");
            { char why[200] = ""; if (!rows_objref_ok("Object", g.mesh, why, sizeof why)) { snprintf(err, (size_t)cap, "grid %s: %s", g.id, why); goto done; } }
            if (jfind(&d, e, "origin") < 0 || !ms_vec(&d, e, "origin", org, 3, NULL) || !ms_vec(&d, e, "cell", cell, 2, cell) || !ms_vec(&d, e, "count", cnt, 2, NULL) || !ms_vec(&d, e, "scale", g.scale, 3, one)) { snprintf(err, (size_t)cap, "grid %s: origin [x,y,z] (centre of the first cell), cell [dx,dy], count [nx,ny], scale [x,y,z]", g.id); goto done; }
            int nx = (int)cnt[0], ny = (int)cnt[1]; if (nx < 1 || ny < 1 || s->nm + nx * ny > MS_MESHES) { snprintf(err, (size_t)cap, "grid %s: count must be >= 1 and all meshes + cells at most %d", g.id, MS_MESHES); goto done; }
            int pat = ms_grid_pattern(ms_str(&d, e, "pattern")); float step = (float)ms_numk(&d, e, "step", 0.5); if (pat < 0) { snprintf(err, (size_t)cap, "grid %s: pattern is none / checker / wave / rows / columns / ring", g.id); goto done; }
            if (!ms_parse_look(&d, e, s, &g, err, cap)) goto done;
            g.yaw = (float)ms_numk(&d, e, "yaw", 0); snprintf(g.st[0].name, sizeof g.st[0].name, "default"); g.st[0].collision = ms_boolk(&d, e, "collision", 0); g.nst = 1; g.grid = s->ngrid + 1;
            float base_off = g.phase_offset;
            for (int iy = 0; iy < ny; iy++) for (int ix = 0; ix < nx; ix++) {
                MsMesh *m = &s->m[s->nm++]; *m = g; snprintf(m->id, sizeof m->id, "%s_%d", g.id, iy * nx + ix);
                m->pos[0] = org[0] + ix * cell[0]; m->pos[1] = org[1] + iy * cell[1]; m->pos[2] = org[2]; m->phase_offset = base_off + ms_grid_phase(pat, ix, iy, nx, ny, step);
            }
            s->ngrid++;
        }
    }
    int es = jfind(&d, o, "events");                                                            // events: start | score >= n | match state | every n seconds -> actions
    if (es >= 0) {
        if (d.n[es].t != J_ARR) { snprintf(err, (size_t)cap, "\"events\" must be an array"); goto done; }
        for (int e = d.n[es].first; e >= 0; e = d.n[e].next) {
            if (s->ne >= MS_EVENTS) { snprintf(err, (size_t)cap, "at most %d events per scene", MS_EVENTS); goto done; } MsEvent *ev = &s->ev[s->ne]; memset(ev, 0, sizeof *ev);
            v = ms_str(&d, e, "when"); static const char *kinds[] = { "start", "score", "state", "every" }; ev->kind = -1; for (int k = 0; k < 4; k++) if (v && !strcmp(v, kinds[k])) ev->kind = k;
            if (ev->kind < 0) { snprintf(err, (size_t)cap, "event %d: \"when\" is start / score / state / every", s->ne + 1); goto done; }
            if (ev->kind == 1) { ev->arg = (float)ms_numk(&d, e, "min", 1); if (ev->arg < 1 || ev->arg > 100) { snprintf(err, (size_t)cap, "event %d: score \"min\" 1..100", s->ne + 1); goto done; } }
            if (ev->kind == 2) { v = ms_str(&d, e, "state"); if (!v || !*v || strlen(v) >= sizeof ev->text) { snprintf(err, (size_t)cap, "event %d: \"state\" text (part of the match state, e.g. InProgress) is required", s->ne + 1); goto done; } snprintf(ev->text, sizeof ev->text, "%s", v); }
            if (ev->kind == 3) { ev->arg = (float)ms_numk(&d, e, "seconds", 0); if (!(ev->arg >= 0.05f && ev->arg <= 3600)) { snprintf(err, (size_t)cap, "event %d: \"seconds\" 0.05..3600", s->ne + 1); goto done; } }
            v = ms_str(&d, e, "if"); snprintf(ev->cond, sizeof ev->cond, "%s", v ? v : "always");
            char er2[160]; if (!ms_parse_acts(&d, jfind(&d, e, "do"), ev->d, &ev->nd, er2, sizeof er2) || !ms_parse_acts(&d, jfind(&d, e, "undo"), ev->u, &ev->nu, er2, sizeof er2)) { snprintf(err, (size_t)cap, "event %d: %s", s->ne + 1, er2); goto done; }
            s->ne++;
        }
    }
    int ts = jfind(&d, o, "triggers");
    if (ts >= 0) {
        if (d.n[ts].t != J_ARR) { snprintf(err, (size_t)cap, "\"triggers\" must be an array"); goto done; }
        for (int e = d.n[ts].first; e >= 0; e = d.n[e].next) {
            if (s->nt >= MS_TRIGS) { snprintf(err, (size_t)cap, "at most %d triggers per scene", MS_TRIGS); goto done; } MsTrig *t = &s->t[s->nt];
            v = ms_str(&d, e, "id"); if (!ms_id_ok(v)) { snprintf(err, (size_t)cap, "trigger %d: bad or missing id", s->nt + 1); goto done; } snprintf(t->id, sizeof t->id, "%s", v);
            int bx = jfind(&d, e, "box"); static const float hd[3] = { 150, 150, 200 };
            if (bx < 0 || d.n[bx].t != J_OBJ || !ms_vec(&d, bx, "center", t->center, 3, NULL) || !ms_vec(&d, bx, "half", t->half, 3, hd)) { snprintf(err, (size_t)cap, "trigger %s: \"box\": {\"center\":[x,y,z],\"half\":[x,y,z]} is required", t->id); goto done; }
            v = ms_str(&d, e, "who"); if (v && strcmp(v, "player")) { snprintf(err, (size_t)cap, "trigger %s: who must be \"player\" (other characters are not supported)", t->id); goto done; }
            t->once = ms_boolk(&d, e, "once", 0);
            char er2[160]; if (!ms_parse_acts(&d, jfind(&d, e, "on_enter"), t->enter, &t->nenter, er2, sizeof er2) || !ms_parse_acts(&d, jfind(&d, e, "on_exit"), t->exit, &t->nexit, er2, sizeof er2)) { snprintf(err, (size_t)cap, "trigger %s: %s", t->id, er2); goto done; }
            s->nt++;
        }
    }
    if (!s->nl && !s->nm && !s->nt && !s->ne && !s->nav) { snprintf(err, (size_t)cap, "an empty scene (no lights, meshes, grids, triggers, events or navmesh)"); goto done; }
    for (int i = 0; i < s->nt + s->ne; i++) for (int w = 0; w < 2; w++) {          // action targets must exist (triggers, then events)
        int isev = i >= s->nt; MsTrig *tr = isev ? NULL : &s->t[i]; MsEvent *ev = isev ? &s->ev[i - s->nt] : NULL; char who[48];
        if (isev) snprintf(who, sizeof who, "event %d", i - s->nt + 1); else snprintf(who, sizeof who, "trigger %s", tr->id);
        int na = isev ? (w ? ev->nu : ev->nd) : (w ? tr->nexit : tr->nenter); MsAct *acts = isev ? (w ? ev->u : ev->d) : (w ? tr->exit : tr->enter);
        for (int a = 0; a < na; a++) {
            MsAct *ac = &acts[a]; int found = 0;
            if (ac->kind == 0) { for (int k = 0; k < s->nm; k++) if (!strcmp(s->m[k].id, ac->target)) { found = 1; int st = 0; for (int q = 0; q < s->m[k].nst; q++) if (!strcmp(s->m[k].st[q].name, ac->state)) st = 1; if (!st) { snprintf(err, (size_t)cap, "%s: mesh %s has no state '%s'", who, ac->target, ac->state); goto done; } } }
            else if (ac->kind == 2) { for (int k = 0; k < s->nl; k++) if (!strcmp(s->l[k].id, ac->target)) found = 1; } else found = 1;
            if (!found) { snprintf(err, (size_t)cap, "%s: action target '%s' does not exist", who, ac->target); goto done; }
        }
    }
    ok = 1;
done:
    jdoc_free(&d); return ok;
}

// ----- content kind "maps"
static int ms_validate(const char *json, char *err, int cap, void *user) { (void)user; MsScene *s = calloc(1, sizeof *s); if (!s) { snprintf(err, (size_t)cap, "out of memory"); return 0; } int ok = ms_parse(json, s, err, cap); free(s); return ok; }
static void ms_destroy_live(MsScene *s);
static int ms_apply(const char *mod_id, const char *id, const char *json, void *user) {
    (void)user; MsScene *s = calloc(1, sizeof *s); char err[200] = ""; if (!s) return 1;
    if (!ms_parse(json, s, err, sizeof err)) { logf_("mapscript: %s/%s: %s", mod_id, id, err); free(s); return 0; }
    snprintf(s->mod, sizeof s->mod, "%s", mod_id);
    for (int i = 0; i < g_ms_n; i++) if (!strcmp(g_ms[i]->id, s->id) && !strcmp(g_ms[i]->mod, s->mod)) {               // reload: replace the definition, rebuild in the running world
        ms_destroy_live(g_ms[i]); free(g_ms[i]); g_ms[i] = s; g_ms_rebuild = 1; logf_("mapscript: scene %s/%s reloaded (%d lights, %d meshes, %d triggers)", mod_id, s->id, s->nl, s->nm, s->nt); return 0;
    }
    if (g_ms_n >= MS_SCENES) { logf_("mapscript: %s/%s: too many scenes (%d)", mod_id, id, MS_SCENES); free(s); return 0; }
    g_ms[g_ms_n++] = s; logf_("mapscript: scene %s/%s registered for maps matching '%s' (%d lights, %d meshes incl. %d grid(s), %d triggers, %d events, %d curves, origin %s, clock %s)", mod_id, s->id, s->map, s->nl, s->nm, s->ngrid, s->nt, s->ne, s->nc, s->relative ? "player" : "world", s->server_clock ? "server" : "local");
    return 0;
}

// ----- engine side
static void *ms_obj(void *o, const char *prop) { void **p = o ? (void **)api_p_prop_ptr(NULL, o, prop) : NULL; return p && se_valid_ptr(*p) ? *p : NULL; }
static int ms_call(void *o, const char *fn, void **args, void *ret) { void *f = o ? api_p_find_function(NULL, o, fn) : NULL; if (!f) { static int n; if (n++ < 20) logf_("mapscript: function %s not found", fn); return 0; } return api_p_call_params(NULL, o, f, args, ret); }
static void ms_set_mobility(void *comp, int mob) {                    // USceneComponent::SetMobility is native only in 4.18: through the PDB symbol table (property write as the fallback)
    uint32_t rva = api_p_find_symbol(NULL, "USceneComponent::SetMobility");
    if (rva) ((void (*)(void *, uint8_t))(g_base + rva))(comp, (uint8_t)mob); else api_p_set_text(NULL, comp, "Mobility", mob == 2 ? "Movable" : "Static");
}
static int ms_actor_move(void *actor, const float to[3]) { uint8_t sweep = 0, tele = 1, ok = 0; static uint8_t hit[512]; void *args[4] = { (void *)to, &sweep, hit, &tele }; ms_call(actor, "K2_SetActorLocation", args, &ok); return ok; }
static void ms_actor_collision(void *actor, int on) { uint8_t b = (uint8_t)(on != 0); void *a[1] = { &b }; ms_call(actor, "SetActorEnableCollision", a, NULL); }
static void ms_light_vis(MsLight *l, int on) { uint8_t vis = (uint8_t)on, prop = 1; void *a[2] = { &vis, &prop }; ms_call(l->comp, "SetVisibility", a, NULL); l->vis = on; }
static void ms_light_apply(MsLight *l, const float c[3], float intensity, const float cal[3]) {   // c / intensity are the authored values; cal (scene calibration) is applied here, cur_* keep the authored ones
    float cc[3]; ms_calibrate(cal, c, cc); float col[4] = { cc[0], cc[1], cc[2], 1 }; uint8_t srgb = 1; void *a1[2] = { col, &srgb }; ms_call(l->comp, "SetLightColor", a1, NULL);
    float ci = intensity * cal[1]; void *a2[1] = { &ci }; ms_call(l->comp, "SetIntensity", a2, NULL); memcpy(l->cur_c, c, sizeof l->cur_c); l->cur_i = intensity;
}
static void ms_destroy_live(MsScene *s) {
    if (!s->live) return;
    for (int i = 0; i < s->nl; i++) if (s->l[i].actor) { ms_call(s->l[i].actor, "K2_DestroyActor", NULL, NULL); s->l[i].actor = s->l[i].comp = NULL; if (g_ms_lights_live > 0) g_ms_lights_live--; }
    for (int i = 0; i < s->nm; i++) if (s->m[i].actor) { ms_call(s->m[i].actor, "K2_DestroyActor", NULL, NULL); s->m[i].actor = s->m[i].comp = s->m[i].mid = NULL; }
    s->live = 0; logf_("mapscript: scene %s destroyed", s->id);
}
static void ms_forget_live(void) {
    for (int i = 0; i < g_ms_n; i++) {
        MsScene *s = g_ms[i]; s->live = 0;
        for (int k = 0; k < s->nl; k++) { s->l[k].actor = s->l[k].comp = NULL; s->l[k].vis = 0; s->l[k].cur_i = -1; }
        for (int k = 0; k < s->nm; k++) { s->m[k].actor = s->m[k].comp = s->m[k].mid = NULL; s->m[k].mv.active = 0; s->m[k].hidden = 0; }
        for (int k = 0; k < s->nt; k++) { s->t[k].inside = 0; s->t[k].fired = 0; }
        for (int k = 0; k < s->ne; k++) { s->ev[k].state = 0; s->ev[k].acc = 0; }
    }
    g_ms_lights_live = 0;
}
static int ms_icontains(const char *hay, const char *needle);
static void ms_mesh_place_state(MsScene *s, MsMesh *m, int st, int instant) {
    static const float zero[3] = { 0, 0, 0 }; float delta[3], target[3]; ms_world_pos(s->relative, zero, s->yaw, m->st[st].off, delta);
    for (int i = 0; i < 3; i++) target[i] = m->base[i] + delta[i];
    int was = m->cur; m->cur = st; float now[3] = { m->base[0], m->base[1], m->base[2] }; if (m->actor) api_p_test_get_location(NULL, m->actor, now);
    if (instant || m->move_time <= 0.001f) { ms_actor_move(m->actor, target); ms_actor_collision(m->actor, m->st[st].collision); m->mv.active = 0; m->pending_col = -1; }
    else { ms_move_start(&m->mv, now, target, m->move_time); if (!m->st[st].collision) { ms_actor_collision(m->actor, 0); m->pending_col = -1; } else m->pending_col = 1; }
    if (!m->grid) logf_("mapscript: mesh %s: state %s -> %s (collision %d, %s)", m->id, m->st[was].name, m->st[st].name, m->st[st].collision, instant ? "instant" : "moving");
}
static int ms_mesh_set_state(MsScene *s, const char *mesh, const char *state) {
    for (int i = 0; i < s->nm; i++) if (!strcmp(s->m[i].id, mesh) && s->m[i].actor) for (int q = 0; q < s->m[i].nst; q++) if (!strcmp(s->m[i].st[q].name, state)) { if (s->m[i].cur != q) ms_mesh_place_state(s, &s->m[i], q, 0); return 1; }
    return 0;
}
static void ms_run_acts(MsScene *s, const char *trig, MsAct *a, int n) {
    for (int i = 0; i < n; i++) switch (a[i].kind) {
        case 0: logf_("mapscript: trigger %s: mesh %s -> %s (%s)", trig, a[i].target, a[i].state, ms_mesh_set_state(s, a[i].target, a[i].state) ? "ok" : "FAILED"); break;
        case 1: ms_phase_set(&g_ms_ph, a[i].target, a[i].on); logf_("mapscript: trigger %s: phase %s = %d", trig, a[i].target, a[i].on); break;
        case 2: for (int k = 0; k < s->nl; k++) if (!strcmp(s->l[k].id, a[i].target)) s->l[k].forced = a[i].on ? 1 : 2; logf_("mapscript: trigger %s: light %s %s", trig, a[i].target, a[i].on ? "on" : "off"); break;
        case 3: api_p_show_banner(NULL, a[i].text, a[i].secs); break;
        case 4: { int k = ms_cycle(&g_ms_ph, a[i].text); logf_("mapscript: %s: cycle [%s] -> #%d", trig, a[i].text, k); break; }
    }
}
static int ms_pawn_pos(float pos[3], float *yaw) {
    void *p = cd_local_pawn(); if (!p || !api_p_test_get_location(NULL, p, pos)) return 0;
    float fw[3]; *yaw = 0; if (api_p_test_get_forward(NULL, p, fw) && (fabsf(fw[0]) + fabsf(fw[1])) > 0.01f) *yaw = (float)(atan2(fw[1], fw[0]) * 57.29577951308232);
    return 1;
}
// ----- runtime navmesh (scene "navmesh"). Duel maps ship no navmesh, so the AI's MoveTo / pathfinding has nothing to walk on.
// The shipping exe still has the full Recast generator: the loader spawns an ANavMeshBoundsVolume, give its brush component a one-box BodySetup (CalcBounds reads
// BrushBodySetup; the volume has no UModel at runtime), report it with UNavigationSystem.OnNavigationBoundsUpdated -> the navigation system spawns the
// RecastNavMesh itself (config RuntimeGeneration = DynamicModifiersOnly, which skips geometry tiles in a game world) -> the loader switches that instance to Dynamic,
// re-populate the nav octree (its geometry export delegate is only bound for a Dynamic MainNavData) and call UNavigationSystem::RebuildAll (PDB symbols) -> async tile build; IsNavigationBeingBuilt false = ready. One navmesh per world (the first scene with "navmesh").
// Callers (bots): ms_nav_ready() / ms_nav_path(); with a ready navmesh AIController.MoveToActor(..., bUsePathfinding = true) follows real paths.
static struct { int st, tries; void *vol, *nav; float c[3], h[3]; double t, t2, t4; char scene[40]; } g_nav;   // st: 0 none, 1 bounds reported, 4 settling, 2 rebuild issued, 3 ready, -1 failed
static void *ms_nav_sys(void) { void *cdo = api_p_find_cdo(NULL, "NavigationSystem"), *w = world(), *r = NULL; void *a[1] = { &w }; if (cdo && w) ms_call(cdo, "GetNavigationSystem", a, &r); return r && se_valid_ptr(r) ? r : NULL; }
static void ms_nav_fail(const char *why) { g_nav.st = -1; logf_("mapscript: navmesh FAILED (%s): %s - AI keeps moving without pathfinding", g_nav.scene, why); }
static int ms_nav_ready(void) { return g_nav.st == 3; }
// 1 = a navmesh point within extent of p (out = the point on the navmesh)
static int ms_nav_project(const float p[3], const float extent[3], float out[3]) {
    void *cdo = api_p_find_cdo(NULL, "NavigationSystem"), *w = world(), *nd = NULL, *flt = NULL; uint8_t ok = 0; float pt[3] = { p[0], p[1], p[2] }, ex[3] = { extent[0], extent[1], extent[2] };
    void *a[6] = { &w, pt, out, &nd, &flt, ex }; if (!cdo || !w || !ms_call(cdo, "K2_ProjectPointToNavigation", a, &ok)) return 0; return ok != 0;
}
// path query for bots: 1 = a path exists (partial = only part of the way); len = path length (uu); npts = path points. Game thread.
static int ms_nav_path(const float from[3], const float to[3], float *len, int *partial, int *npts) {
    *len = -1; *partial = 0; *npts = 0; if (g_nav.st != 3) return 0;
    void *cdo = api_p_find_cdo(NULL, "NavigationSystem"), *w = world(), *ctx = NULL, *flt = NULL, *path = NULL; float a0[3] = { from[0], from[1], from[2] }, a1[3] = { to[0], to[1], to[2] };
    void *a[5] = { &w, a0, a1, &ctx, &flt }; if (!cdo || !w || !ms_call(cdo, "FindPathToLocationSynchronously", a, &path) || !path || !se_valid_ptr(path)) return 0;
    uint8_t valid = 0, part = 0; ms_call(path, "IsValid", NULL, &valid); ms_call(path, "IsPartial", NULL, &part); ms_call(path, "GetPathLength", NULL, len);
    struct { void *data; int32_t num, max; } *pts = api_p_prop_ptr(NULL, path, "PathPoints"); if (pts) *npts = pts->num;
    *partial = part != 0; return valid != 0;
}
// state dump (PDB layouts): UNavigationSystem NavDataSet +0x68, PendingNavBoundsUpdates +0xD8, RegisteredNavBounds +0x190,
// NavOctree +0x238, NavBuildingLockFlags +0x3F8; ARecastNavMesh generator +0x438; FRecastNavMeshGenerator TotalNavBounds +0xAC, InclusionBounds +0xC8,
// PendingDirtyTiles +0xE0, RunningDirtyTiles +0xF0. Read-only, for the log.
static void ms_nav_diag(const char *tag, void *sys) {
    __try {
        uint8_t *y = sys; uint8_t *gen = g_nav.nav ? *(uint8_t **)((uint8_t *)g_nav.nav + 0x438) : NULL;
        logf_("mapscript: navmesh diag %s: nav data %d, pending bounds %d, registered bounds slots %d, octree %p, build lock 0x%02X; generator %p", tag, *(int *)(y + 0x70), *(int *)(y + 0xE0), *(int *)(y + 0x198), *(void **)(y + 0x238), y[0x3F8], gen);
        if (gen && se_valid_ptr(gen)) { float *b = (float *)(gen + 0xAC);
            logf_("mapscript: navmesh diag %s: generator total bounds %.0f %.0f %.0f .. %.0f %.0f %.0f (valid %d), inclusion bounds %d, pending tiles %d, running tiles %d", tag, b[0], b[1], b[2], b[3], b[4], b[5], *(uint8_t *)(gen + 0xC4), *(int *)(gen + 0xD0), *(int *)(gen + 0xE8), *(int *)(gen + 0xF8)); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf_("mapscript: navmesh diag %s: EXCEPTION", tag); }
}
static void ms_nav_start(const MsScene *s) {
    if (g_nav.st) return;                                                                       // one per world
    memset(&g_nav, 0, sizeof g_nav); memcpy(g_nav.c, s->nav_c, sizeof g_nav.c); memcpy(g_nav.h, s->nav_h, sizeof g_nav.h); snprintf(g_nav.scene, sizeof g_nav.scene, "%s", s->id); g_nav.t = g_ms_clock;
    void *sys = ms_nav_sys(); if (!sys) { ms_nav_fail("this world has no navigation system"); return; }
    { void *nav0 = ms_obj(sys, "MainNavData"); if (nav0) logf_("mapscript: navmesh: the world already has navigation data %p (MainNavData) - the box is added to it", nav0); }
    void *vcls = api_p_find_class(NULL, "NavMeshBoundsVolume"), *bcls = api_p_find_class(NULL, "BodySetup"); float p0[3] = { g_nav.c[0], g_nav.c[1], g_nav.c[2] + 10 }, r0[3] = { 0, 0, 0 };
    g_nav.vol = vcls ? api_p_spawn_actor(NULL, vcls, p0, r0) : NULL; void *bc = ms_obj(g_nav.vol, "BrushComponent"), *bs = bcls && g_nav.vol ? api_p_construct_object(NULL, bcls, g_nav.vol) : NULL;
    void **slot = bc ? (void **)api_p_prop_ptr(NULL, bc, "BrushBodySetup") : NULL; char box[160] = "";
    if (!g_nav.vol || !bc || !bs || !slot) { logf_("mapscript: navmesh: class %p volume %p brush component %p body setup class %p body setup %p slot %p", vcls, g_nav.vol, bc, bcls, bs, slot); ms_nav_fail("bounds volume could not be made"); return; }
    if (!ms_nav_aggeom_text(g_nav.h, box, sizeof box) || !api_p_set_text(NULL, bs, "AggGeom", box)) { ms_nav_fail("BodySetup.AggGeom import failed"); return; }
    *slot = bs; ms_set_mobility(bc, 2); ms_actor_move(g_nav.vol, g_nav.c);                      // the move re-runs UpdateComponentToWorld -> bounds from the new body setup (a static component would refuse the move)
    float bo[3] = { 0, 0, 0 }, be[3] = { 0, 0, 0 }; uint8_t only = 0; void *c[3] = { &only, bo, be }; ms_call(g_nav.vol, "GetActorBounds", c, NULL);
    logf_("mapscript: navmesh (%s): bounds volume %p at %.0f %.0f %.0f, AggGeom %s -> actor bounds centre %.0f %.0f %.0f half %.0f %.0f %.0f", g_nav.scene, g_nav.vol, g_nav.c[0], g_nav.c[1], g_nav.c[2], box, bo[0], bo[1], bo[2], be[0], be[1], be[2]);
    if (be[0] < 50 || be[1] < 50) { ms_nav_fail("the volume has no size (BrushBodySetup bounds not taken)"); return; }
    void *a[1] = { &g_nav.vol }; if (!ms_call(sys, "OnNavigationBoundsUpdated", a, NULL)) { ms_nav_fail("OnNavigationBoundsUpdated not callable"); return; }
    g_nav.st = 1; logf_("mapscript: navmesh: bounds reported to the navigation system %p; waiting for its RecastNavMesh", sys);
}
static void ms_nav_tick(void) {
    if (g_nav.st != 1 && g_nav.st != 2 && g_nav.st != 4) return;
    void *sys = ms_nav_sys(); double el = g_ms_clock - g_nav.t; if (!sys) { ms_nav_fail("navigation system gone"); return; }
    if (g_nav.st == 1) {
        void *nav = ms_obj(sys, "MainNavData");
        if (!nav || !api_p_is_a(NULL, nav, "RecastNavMesh")) { if (el > 10) { logf_("mapscript: navmesh: MainNavData %p after %.1f s", nav, el); ms_nav_fail("the navigation system made no RecastNavMesh"); } return; }
        char gen[40] = "?"; api_p_get_text(NULL, nav, "RuntimeGeneration", gen, sizeof gen);
        int set = api_p_set_text(NULL, nav, "RuntimeGeneration", "Dynamic"); char now[40] = "?"; api_p_get_text(NULL, nav, "RuntimeGeneration", now, sizeof now);
        {   // agent = the player's capsule and step height (config default 34 / 144 / 35: a fatter Absolver capsule then sticks at corners the navmesh lets through)
            void *pw = cd_local_pawn(), *cap = pw ? ms_obj(pw, "CapsuleComponent") : NULL, *mov = pw ? ms_obj(pw, "CharacterMovement") : NULL;
            float *cr = cap ? api_p_prop_ptr(NULL, cap, "CapsuleRadius") : NULL, *ch = cap ? api_p_prop_ptr(NULL, cap, "CapsuleHalfHeight") : NULL, *cs = mov ? api_p_prop_ptr(NULL, mov, "MaxStepHeight") : NULL;
            float *ar = api_p_prop_ptr(NULL, nav, "AgentRadius"), *ah = api_p_prop_ptr(NULL, nav, "AgentHeight"), *as = api_p_prop_ptr(NULL, nav, "AgentMaxStepHeight");
            uint32_t cg = api_p_find_symbol(NULL, "ARecastNavMesh::ConditionalConstructGenerator");
            if (cr && ch && cs && ar && ah && as && cg && *cr > 10 && *cr < 200 && *ch > 30 && *ch < 300) {
                logf_("mapscript: navmesh agent: navmesh radius %.0f height %.0f step %.0f -> player capsule radius %.0f half-height %.0f, step %.0f", *ar, *ah, *as, *cr, *ch, *cs);
                *ar = *cr + MS_NAV_RADIUS_MARGIN; *ah = 2 * *ch; *as = *cs > 0 && *cs < 100 ? *cs : *as;
                ((void (*)(void *))(g_base + cg))(nav);                                         // new generator with the new agent config
            } else logf_("mapscript: navmesh agent: player capsule / movement / symbol not found (%p %p %p %X) - config agent kept", cr, ch, cs, cg);
        }
        g_nav.nav = nav; g_nav.st = 4; g_nav.t4 = g_ms_clock;
        logf_("mapscript: navmesh: RecastNavMesh %p after %.1f s, RuntimeGeneration %s -> %s (set %d); letting the navigation system take the bounds (1 s)", nav, el, gen, now, set);
        ms_nav_diag("found", sys); return;
    }
    if (g_nav.st == 4) {                                                                        // the bounds request is processed in UNavigationSystem::Tick (generator InclusionBounds); rebuild after that
        if (g_ms_clock - g_nav.t4 < 1.0) return;
        ms_nav_diag("before rebuild", sys);
        if (!g_nav.tries) {                                                                     // the octree was filled while the nav data was DynamicModifiersOnly: ConditionalPopulateNavOctree binds the geometry
            uint32_t pr = api_p_find_symbol(NULL, "UNavigationSystem::ConditionalPopulateNavOctree");   // export delegate only when MainNavData is Dynamic, so without a re-population every tile is empty
            if (!pr) { ms_nav_fail("symbol UNavigationSystem::ConditionalPopulateNavOctree missing (symbols.bin)"); return; }
            uint8_t r = ((uint8_t (*)(void *))(g_base + pr))(sys); logf_("mapscript: navmesh: nav octree re-populated with geometry export -> %d", r); ms_nav_diag("octree re-populated", sys);
        }
        uint32_t rva = api_p_find_symbol(NULL, "UNavigationSystem::RebuildAll"); if (!rva) { ms_nav_fail("symbol UNavigationSystem::RebuildAll missing (symbols.bin)"); return; }
        ((void (*)(void *, uint8_t))(g_base + rva))(sys, 0);
        g_nav.st = 2; g_nav.t2 = g_ms_clock; logf_("mapscript: navmesh: RebuildAll issued (try %d)", g_nav.tries + 1); ms_nav_diag("after rebuild call", sys);
        return;
    }
    uint8_t busy = 1; void *cdo = api_p_find_cdo(NULL, "NavigationSystem"), *w = world(); void *a[1] = { &w }; if (cdo) ms_call(cdo, "IsNavigationBeingBuilt", a, &busy);
    double bt = g_ms_clock - g_nav.t2; if (bt < 0.3 || (busy && bt < 60)) return;
    g_nav.st = 3; ms_nav_diag("built", sys);
    float pp[3], yw, ex[3] = { 60, 60, 300 }, on[3] = { 0, 0, 0 }; int ok = -1;
    if (ms_pawn_pos(pp, &yw)) { ok = ms_nav_project(pp, ex, on); logf_("mapscript: navmesh self-check: player %.0f %.0f %.0f -> %s %.0f %.0f %.0f", pp[0], pp[1], pp[2], ok ? "on the navmesh at" : "NOT on the navmesh", on[0], on[1], on[2]); }
    if (ok == 0 && ++g_nav.tries < 3) {                                                         // empty build: report the bounds again and rebuild once they are taken
        void *a[1] = { &g_nav.vol }; ms_call(sys, "OnNavigationBoundsUpdated", a, NULL); g_nav.st = 4; g_nav.t4 = g_ms_clock;
        logf_("mapscript: navmesh: the build holds no navmesh under the player - bounds reported again, rebuild in 1 s (try %d of 3)", g_nav.tries + 1); return;
    }
    logf_("mapscript: navmesh %s (%s): built in %.2f s (%.2f s after the bounds report, %d rebuild(s))%s", ok == 0 ? "BUILT BUT EMPTY under the player" : "READY", g_nav.scene, bt, el, g_nav.tries + 1, busy ? " - still busy after 60 s" : "");
    if (ok == 0) g_nav.st = -1;                                                                 // not usable: bots keep their path-less movement
}
static void ms_build(MsScene *s, const float ppos[3], float pyaw) {
    s->live = 1; s->yaw = s->relative ? pyaw : 0; if (s->relative) memcpy(s->origin, ppos, sizeof s->origin); else memcpy(s->origin, s->offset, sizeof s->origin);
    int made_l = 0, made_m = 0, skipped = 0;
    float score[MS_LIGHTS]; unsigned char keep[MS_LIGHTS];                                 // budget: the strongest (intensity x radius) lights win when the total cap is reached
    for (int i = 0; i < s->nl; i++) score[i] = s->l[i].intensity * s->l[i].radius;
    int live0 = g_ms_lights_live; ms_rank_keep(score, s->nl, MS_MAX_LIGHTS_TOTAL - live0, keep);
    for (int i = 0; i < s->nl; i++) {
        MsLight *l = &s->l[i]; l->cur_i = -1; l->forced = 0; l->vis = 0;
        if (!keep[i]) { skipped++; logf_("mapscript: light %s skipped (budget: %d lights in total over all scenes, %d were live before this scene; the strongest win)", l->id, MS_MAX_LIGHTS_TOTAL, live0); continue; }
        float wp[3]; ms_world_pos(s->relative, s->origin, s->yaw, l->pos, wp); float rot[3] = { l->type == 's' ? l->pitch + 90.f : 0.f, l->yaw + s->yaw, 0 };   // ASpotLight's root has a native -90 pitch: +90 makes "rot" describe the beam (pitch 0 = horizontal)
        void *cls = api_p_find_class(NULL, l->type == 's' ? "SpotLight" : "PointLight"); l->actor = cls ? api_p_spawn_actor(NULL, cls, wp, rot) : NULL;
        l->comp = ms_obj(l->actor, l->type == 's' ? "SpotLightComponent" : "PointLightComponent"); if (!l->comp) l->comp = ms_obj(l->actor, "LightComponent");
        if (!l->comp) { logf_("mapscript: light %s: spawn failed (class %p actor %p)", l->id, cls, l->actor); if (l->actor) ms_call(l->actor, "K2_DestroyActor", NULL, NULL); l->actor = NULL; continue; }
        ms_set_mobility(l->comp, 2); { float r = l->radius; void *a[1] = { &r }; ms_call(l->comp, "SetAttenuationRadius", a, NULL); }
        { uint8_t sh = (uint8_t)l->shadows; void *a[1] = { &sh }; ms_call(l->comp, "SetCastShadows", a, NULL); }
        if (l->type == 's') { float in = l->cone_inner, out = l->cone_outer; void *a[1] = { &out }, *b[1] = { &in }; ms_call(l->comp, "SetOuterConeAngle", a, NULL); ms_call(l->comp, "SetInnerConeAngle", b, NULL); }
        ms_light_apply(l, l->color, l->intensity, s->cal); g_ms_lights_live++; made_l++;
    }
    for (int i = 0; i < s->nm; i++) {
        MsMesh *m = &s->m[i]; m->cur = m->initial; m->pending_col = -1; m->mv.active = 0; ms_world_pos(s->relative, s->origin, s->yaw, m->pos, m->base);
        void *cls = api_p_find_class(NULL, "StaticMeshActor"), *mesh = api_p_load_object(NULL, m->mesh); float rot[3] = { 0, m->yaw + s->yaw, 0 };
        m->actor = cls ? api_p_spawn_actor(NULL, cls, m->base, rot) : NULL; m->comp = ms_obj(m->actor, "StaticMeshComponent");
        if (!m->comp || !mesh) { logf_("mapscript: mesh %s: spawn failed (class %p actor %p component %p mesh %p '%s')", m->id, cls, m->actor, m->comp, mesh, m->mesh); if (m->actor) ms_call(m->actor, "K2_DestroyActor", NULL, NULL); m->actor = m->comp = NULL; continue; }
        ms_set_mobility(m->comp, 2); uint8_t ok = 0; void *a[1] = { &mesh }; ms_call(m->comp, "SetStaticMesh", a, &ok);
        void *b[1] = { m->scale }; ms_call(m->actor, "SetActorScale3D", b, NULL);
        void *mat = m->material[0] ? api_p_load_object(NULL, m->material) : NULL; int idx = 0;
        if (m->material[0] && !mat) logf_("mapscript: mesh %s: material '%s' did not load (mesh keeps its own)", m->id, m->material);
        if (m->tinted) {                                                                    // a dynamic instance of the material (or of the mesh's own) whose vector parameter is driven
            void *c[3] = { &idx, &mat, NULL }; ms_call(m->comp, "CreateDynamicMaterialInstance", c, &m->mid); api_p_fname_make(NULL, m->param, &m->pname); m->cur_c[0] = -1;
            if (!m->mid && !m->grid) logf_("mapscript: mesh %s: CreateDynamicMaterialInstance failed (no colour)", m->id);
        } else if (mat) { void *c[2] = { &idx, &mat }; ms_call(m->comp, "SetMaterial", c, NULL); }
        ms_mesh_place_state(s, m, m->initial, 1); made_m++;
        if (!m->grid || !strcmp(m->id + strlen(m->id) - 2, "_0")) {                          // grids: log the first cell only
            float bo[3] = { 0, 0, 0 }, be[3] = { 0, 0, 0 }; uint8_t only = 0; void *c[3] = { &only, bo, be }; ms_call(m->actor, "GetActorBounds", c, NULL);
            logf_("mapscript: mesh %s spawned at %.0f %.0f %.0f (SetStaticMesh %u, state %s, material %s, colour %s) bounds centre %.0f %.0f %.0f half-size %.0f %.0f %.0f", m->id, m->base[0], m->base[1], m->base[2], ok, m->st[m->initial].name,
                  mat ? m->material : "(own)", m->mid ? m->param : "-", bo[0], bo[1], bo[2], be[0], be[1], be[2]);
        }
    }
    for (int i = 0; i < s->nt; i++) { s->t[i].inside = 0; s->t[i].fired = 0; }
    for (int i = 0; i < s->ne; i++) { s->ev[i].state = 0; s->ev[i].acc = 0; }
    if (s->nav) ms_nav_start(s);
    logf_("mapscript: scene %s/%s built in %s: %d of %d lights (%d skipped by the budget, %d live in total), %d of %d meshes (%d grid(s)), %d triggers, %d events (origin %.0f %.0f %.0f yaw %.0f, %s)", s->mod, s->id, g_ms_world_path, made_l, s->nl, skipped, g_ms_lights_live, made_m, s->nm, s->ngrid, s->nt, s->ne,
          s->origin[0], s->origin[1], s->origin[2], s->yaw, s->relative ? "relative to the player" : "world coordinates");
}
static int ms_icontains(const char *hay, const char *needle) { size_t n = strlen(needle); for (; *hay; hay++) if (!_strnicmp(hay, needle, n)) return 1; return 0; }
// 1 = this world has a scene with a "navmesh" that is not built yet (the navmesh is expected but not started): bots must not spawn into a world without path data
static int ms_nav_expected(void) {
    if (g_nav.st || !g_ms_world_path[0]) return 0;
    for (int i = 0; i < g_ms_n; i++) { const MsScene *s = g_ms[i]; if (s->nav && !s->live && (s->map[0] == '*' || ms_icontains(g_ms_world_path, s->map))) return 1; }
    return 0;
}
static double ms_now(const MsScene *s) {
    if (s->server_clock) { void *gs = md_gs(); void *f = gs ? api_p_find_function(NULL, gs, "GetServerWorldTimeSeconds") : NULL; float t = 0; if (f && api_p_call_params(NULL, gs, f, NULL, &t)) return t; }
    return g_ms_clock;
}
static const MsCurve *ms_curve(const MsScene *s, const char *id) { for (int i = 0; i < s->nc; i++) if (!strcmp(s->c[i].id, id)) return &s->c[i]; return NULL; }
static void ms_update_scene(MsScene *s, float dt, const float ppos[3], int have_pos) {
    double now = ms_now(s);
    for (int i = 0; i < s->nl; i++) {
        MsLight *l = &s->l[i]; if (!l->comp) continue;
        int want = l->forced == 1 ? 1 : l->forced == 2 ? 0 : ms_cond(&g_ms_ph, l->on); if (want != l->vis) { ms_light_vis(l, want); logf_("mapscript: light %s %s", l->id, want ? "on" : "off"); }
        if (!want) continue;
        float c[3], f[3], inten = l->intensity; memcpy(c, l->color, sizeof c);
        if (l->color_curve[0]) { const MsCurve *cc = ms_curve(s, l->color_curve); if (cc) ms_curve_eval(cc, now + l->phase_offset, c); }
        if (l->intensity_curve[0]) { const MsCurve *ic = ms_curve(s, l->intensity_curve); if (ic) { ms_curve_eval(ic, now + l->phase_offset, f); inten = l->intensity * f[0]; } }
        if (l->cur_i < 0 || fabsf(c[0] - l->cur_c[0]) > 1 / 255.f || fabsf(c[1] - l->cur_c[1]) > 1 / 255.f || fabsf(c[2] - l->cur_c[2]) > 1 / 255.f || fabsf(inten - l->cur_i) > 0.005f * (l->intensity > 1 ? l->intensity : 1)) ms_light_apply(l, c, inten, s->cal);
    }
    for (int i = 0; i < s->nm; i++) {
        MsMesh *m = &s->m[i]; if (!m->actor) continue; float p[3];
        if (m->mv.active) { int moving = ms_move_step(&m->mv, dt, p); ms_actor_move(m->actor, p); if (!moving && m->pending_col >= 0) { ms_actor_collision(m->actor, m->pending_col); m->pending_col = -1; } }
        int hide = !ms_cond(&g_ms_ph, m->on);
        if (hide != m->hidden) {                                                            // "on" condition: hidden meshes also lose their collision
            uint8_t h = (uint8_t)hide; void *a[1] = { &h }; ms_call(m->actor, "SetActorHiddenInGame", a, NULL); ms_actor_collision(m->actor, hide ? 0 : m->st[m->cur].collision); m->hidden = hide;
            if (!m->grid || !strcmp(m->id + strlen(m->id) - 2, "_0")) logf_("mapscript: mesh %s%s %s", m->id, m->grid ? " (grid)" : "", hide ? "hidden" : "shown");
        }
        if (m->mid && !hide) {
            float c[3] = { m->color[0], m->color[1], m->color[2] }; if (m->color_curve[0]) { const MsCurve *cc = ms_curve(s, m->color_curve); if (cc) ms_curve_eval(cc, now + m->phase_offset, c); }
            { float cc[3]; ms_calibrate(s->cal, c, cc); for (int k = 0; k < 3; k++) c[k] = cc[k]; }   // saturation + gamma only (brightness is the author's glow)
            for (int k = 0; k < 3; k++) c[k] *= m->brightness;
            if (fabsf(c[0] - m->cur_c[0]) > 1 / 255.f || fabsf(c[1] - m->cur_c[1]) > 1 / 255.f || fabsf(c[2] - m->cur_c[2]) > 1 / 255.f) {
                float col[4] = { c[0], c[1], c[2], 1 }; void *a[2] = { &m->pname, col }; ms_call(m->mid, "SetVectorParameterValue", a, NULL); memcpy(m->cur_c, c, sizeof m->cur_c);
            }
        }
    }
    void *gs = NULL; int score = -1; char mstate[64] = "";                                // events (score / state read lazily, once per tick)
    for (int i = 0; i < s->ne; i++) {
        MsEvent *ev = &s->ev[i]; char who[16]; snprintf(who, sizeof who, "event %d", i + 1); int now_on = 0;
        if (ev->kind == 0) { if (!ev->state) { ev->state = 1; logf_("mapscript: %s (start) fired", who); ms_run_acts(s, who, ev->d, ev->nd); } continue; }
        if (ev->kind == 3) { if (ms_every(&ev->acc, dt, ev->arg) && ms_cond(&g_ms_ph, ev->cond)) ms_run_acts(s, who, ev->d, ev->nd); continue; }
        if (!gs) gs = md_gs();
        if (ev->kind == 1) { if (score < 0 && gs) { int a0 = md_team_score(gs, 0), a1 = md_team_score(gs, 1); score = a0 >= 0 && a1 >= 0 ? a0 + a1 : -1; } now_on = score >= (int)ev->arg; }
        else { if (!mstate[0] && gs) md_state_text(gs, mstate, sizeof mstate); now_on = mstate[0] && ms_icontains(mstate, ev->text); }
        int e = ms_edge(&ev->state, now_on && ms_cond(&g_ms_ph, ev->cond));
        if (e == 1) { logf_("mapscript: %s (%s) true (score %d, state '%s')", who, ev->kind == 1 ? "score" : "state", score, mstate); ms_run_acts(s, who, ev->d, ev->nd); }
        else if (e == -1) { logf_("mapscript: %s (%s) false again (score %d, state '%s')", who, ev->kind == 1 ? "score" : "state", score, mstate); ms_run_acts(s, who, ev->u, ev->nu); }
    }
    if (!have_pos) return;
    for (int i = 0; i < s->nt; i++) {
        MsTrig *t = &s->t[i]; if (t->once && t->fired) continue; float c[3]; ms_world_pos(s->relative, s->origin, s->yaw, t->center, c);
        float d[3] = { ppos[0] - c[0], ppos[1] - c[1], ppos[2] - c[2] }, q[3] = { d[0], d[1], d[2] }, zero[3] = { 0, 0, 0 };
        if (s->relative && s->yaw != 0) { double a = -s->yaw * 3.14159265358979 / 180.0, co = cos(a), si = sin(a); q[0] = (float)(d[0] * co - d[1] * si); q[1] = (float)(d[0] * si + d[1] * co); }   // the box turns with the player's yaw
        int e = ms_edge(&t->inside, ms_in_box(q, zero, t->half));
        if (e == 1) { t->fired = 1; logf_("mapscript: trigger %s entered at %.0f %.0f %.0f", t->id, ppos[0], ppos[1], ppos[2]); ms_run_acts(s, t->id, t->enter, t->nenter); }
        else if (e == -1) { logf_("mapscript: trigger %s left", t->id); ms_run_acts(s, t->id, t->exit, t->nexit); }
    }
}
static void ms_tick(float dt) {
    if (!g_ms_n) return;
    void *w = world();
    if (g_se_world_gen != g_ms_world) {                                 // new world (secore generation: also a new world at the old address): the old actors went with it
        g_ms_world = g_se_world_gen; ms_forget_live(); g_ms_wait = 0; memset(&g_nav, 0, sizeof g_nav); g_ms_world_path[0] = 0; g_ms_rebuild = 0; memset(&g_ms_ph, 0, sizeof g_ms_ph);
        if (w) { __try { se_path_of(w, g_ms_world_path, sizeof g_ms_world_path); } __except (EXCEPTION_EXECUTE_HANDLER) { g_ms_world_path[0] = 0; } logf_("mapscript: world %s", g_ms_world_path); }
    }
    if (!w || !g_ms_world_path[0]) return;
    g_ms_clock += dt; g_ms_tick_n++;
    __try {
        float ppos[3] = { 0, 0, 0 }, pyaw = 0; int have = L_local_pc() ? ms_pawn_pos(ppos, &pyaw) : 0;
        if (have) g_ms_wait += dt;                                                                          // the delay counts from the moment the player exists (once per tick: scenes build in delay order)
        for (int i = 0; i < g_ms_n; i++) {
            MsScene *s = g_ms[i];
            if (!s->live) {
                if (!(s->map[0] == '*' || ms_icontains(g_ms_world_path, s->map))) continue;
                if (!have || g_ms_wait < s->delay) continue;
                ms_build(s, ppos, pyaw);
            }
            ms_update_scene(s, dt, ppos, have);
        }
        ms_nav_tick();
        g_ms_rebuild = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { static int n; if (n++ < 5) logf_("mapscript: tick EXCEPTION 0x%08lX", GetExceptionCode()); }
}
// ----- plugin API
static int api_p_mapscript_phase(const AbsApi *self, const char *name, int on) { int ok = ms_phase_set(&g_ms_ph, name, on); logf_("plugin %s: mapscript_phase('%s', %d) -> %d", plug_name_of(plug_of(self)), name ? name : "", on, ok); return ok; }
static int api_p_mapscript_mesh(const AbsApi *self, const char *mesh, const char *state) {
    int ok = 0; if (mesh && state) for (int i = 0; i < g_ms_n && !ok; i++) if (g_ms[i]->live) ok = ms_mesh_set_state(g_ms[i], mesh, state);
    logf_("plugin %s: mapscript_mesh('%s', '%s') -> %d", plug_name_of(plug_of(self)), mesh ? mesh : "", state ? state : "", ok); return ok;
}
static void ms_fill_api(AbsApi *a) { a->mapscript_phase = api_p_mapscript_phase; a->mapscript_mesh = api_p_mapscript_mesh; }
