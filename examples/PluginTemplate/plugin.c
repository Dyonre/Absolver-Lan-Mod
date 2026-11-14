/* PluginTemplate: the smallest AbsolverSE (API v2) plugin. Copy this folder, rename, build with build.bat, put plugin.dll in <game>\Mods\<YourName>\ and
 * remove the # before the name in Mods\plugins.txt. See docs\PLUGIN-API.md. */
#include <windows.h>
#include "../../tools/lannative/AbsolverSE.h"

static const AbsApi *A;

static void on_ready(void *user) { (void)user; A->log(A, "engine is ready"); }
static void on_tick(float dt, void *user) { (void)dt; (void)user; /* game thread, every frame: keep it cheap */ }

__declspec(dllexport) int AbsPlugin_Load(const AbsApi *api) {
    if (api->version != ABS_API_VERSION || api->size < sizeof(AbsApi)) return 1;     /* a plugin built for another API version stays off */
    A = api;
    A->log(A, "%s loaded from %s", api->plugin_name, api->plugin_dir);
    A->on_engine_ready(A, on_ready, NULL);
    A->on_tick(A, on_tick, NULL);
    return 0;                                                                         /* 0 = ok */
}
