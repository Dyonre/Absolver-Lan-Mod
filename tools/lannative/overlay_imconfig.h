// overlay_imconfig.h - Dear ImGui build options for the loader's overlay, force-included into every translation unit that includes ImGui headers (build.bat: /FI).
// The vendored ImGui sources stay unmodified; everything is configured from here.
#ifndef LANNATIVE_OVERLAY_IMCONFIG_H
#define LANNATIVE_OVERLAY_IMCONFIG_H
#define IM_ASSERT(_EXPR) ((void)0)                       // a bad call must never abort the game process; the frame guard in overlay.cpp catches real faults
#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD                 // the overlay never reads the pad (no XInput load)
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS            // no shell32 (OpenInShell)
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS        // no imm32
#define IMGUI_DISABLE_DEMO_WINDOWS                       // the demo / debug windows are not part of the build
#define IMGUI_DISABLE_DEBUG_TOOLS
#endif
