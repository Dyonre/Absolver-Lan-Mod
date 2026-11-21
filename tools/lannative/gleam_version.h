// Gleam Loader version. The log's first line, the DLL's version information and the installer all read it from here.
#pragma once
#define GLEAM_VER_MAJOR 5
#define GLEAM_VER_MINOR 3
#define GLEAM_VER_PATCH 0
#define GLEAM_VERSION "5.3.0"
// Joins need the same MAJOR version on both sides (lanmods.h lm_ver_refuse): bump the major only for a change that older loaders cannot play with.
