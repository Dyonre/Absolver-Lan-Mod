/* HelloPlugin: the smallest useful plugin. It logs when the engine is ready, looks up the attack table through reflection,
 * counts widgets once, and logs a heartbeat every 600 frames. Build: build.bat. Install: copy plugin.dll to Mods\HelloPlugin\ and
 * remove the # before HelloPlugin in Mods\plugins.txt. */
#include <windows.h>
#include <stdio.h>
#include "../../tools/lannative/AbsolverSE.h"

static const AbsApi *A;
static int frames;

static void count_widget(void *obj, void *user) { if (A->is_a(A, obj, "UserWidget")) (*(int *)user)++; }

static void on_ready(void *user) {
    (void)user;
    void *t = A->find_object(A, "/Game/DB/Attacks/attacks.attacks");
    char cls[64] = "?", nm[64] = "?";
    if (t) { A->class_name(A, A->object_class(A, t), cls, sizeof cls); A->object_name(A, t, nm, sizeof nm); }
    A->log(A, "engine ready; attack table %s (class %s) at %p", nm, cls, t);
    int widgets = 0;
    int total = A->for_each_object(A, count_widget, &widgets);
    A->log(A, "%d live objects, %d of them widgets", total, widgets);
    char v[64];
    if (A->config_get(A, "greeting", v, sizeof v)) A->log(A, "plugin.cfg greeting = %s", v);
}
static void on_tick(float dt, void *user) { (void)dt; (void)user; if (++frames % 600 == 0) A->log(A, "heartbeat: %d frames", frames); }

__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION || api->size < sizeof(AbsApi)) return 1;
    A = api;
    A->log(A, "hello from %s (game base %p, folder %s)", api->plugin_name, (void *)api->game_base, api->plugin_dir);
    A->on_engine_ready(A, on_ready, NULL);
    A->on_tick(A, on_tick, NULL);
    return 0;
}

