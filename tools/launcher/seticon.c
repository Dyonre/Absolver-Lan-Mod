/* seticon <src.exe> <dst.exe>: copies the first icon group (and its images) of src into dst as icon group 1.
 * Used by build.bat to give the launcher the game's own mask icon, taken from the installed
 * Absolver-Win64-Shipping.exe at build time (the icon is a game asset, so it is never committed).
 * Without it, Windows shows the default blue app icon for Absolver.exe (= this launcher). */
#include <windows.h>
#include <stdio.h>

#pragma pack(push, 2)
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD bytes; WORD id; } GrpEntry;   /* GRPICONDIRENTRY */
typedef struct { WORD res, type, count; GrpEntry e[1]; } GrpDir;
#pragma pack(pop)

static LPWSTR g_first;
static BOOL CALLBACK first_group(HMODULE m, LPCWSTR type, LPWSTR name, LONG_PTR p) {
    (void)m; (void)type; (void)p;
    g_first = IS_INTRESOURCE(name) ? name : _wcsdup(name);
    return FALSE;
}

static const void *res(HMODULE m, LPCWSTR name, LPCWSTR type, DWORD *size) {
    HRSRC r = FindResourceW(m, name, type);
    if (!r) return NULL;
    *size = SizeofResource(m, r);
    return LockResource(LoadResource(m, r));
}

int wmain(int argc, wchar_t **argv) {
    if (argc != 3) { fprintf(stderr, "usage: seticon <src.exe> <dst.exe>\n"); return 2; }
    HMODULE src = LoadLibraryExW(argv[1], NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!src) { fprintf(stderr, "seticon: cannot open %ls (error %lu)\n", argv[1], GetLastError()); return 1; }
    EnumResourceNamesW(src, (LPCWSTR)RT_GROUP_ICON, first_group, 0);
    DWORD gsize = 0;
    const GrpDir *g = g_first ? res(src, g_first, (LPCWSTR)RT_GROUP_ICON, &gsize) : NULL;
    if (!g || gsize < 6 || gsize < 6 + (DWORD)g->count * sizeof(GrpEntry)) { fprintf(stderr, "seticon: %ls has no icon group\n", argv[1]); return 1; }

    HANDLE up = BeginUpdateResourceW(argv[2], FALSE);
    if (!up) { fprintf(stderr, "seticon: cannot update %ls (error %lu)\n", argv[2], GetLastError()); return 1; }
    for (int i = 0; i < g->count; i++) {
        DWORD isize = 0; WORD id = g->e[i].id;
        const void *img = res(src, MAKEINTRESOURCEW(id), (LPCWSTR)RT_ICON, &isize);
        if (!img || !UpdateResourceW(up, (LPCWSTR)RT_ICON, MAKEINTRESOURCEW(id), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (void *)img, isize)) {
            fprintf(stderr, "seticon: icon image %u failed (error %lu)\n", id, GetLastError()); EndUpdateResourceW(up, TRUE); return 1;
        }
    }
    if (!UpdateResourceW(up, (LPCWSTR)RT_GROUP_ICON, MAKEINTRESOURCEW(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (void *)g, gsize) || !EndUpdateResourceW(up, FALSE)) {
        fprintf(stderr, "seticon: writing %ls failed (error %lu)\n", argv[2], GetLastError()); return 1;
    }
    printf("seticon: %d icon image(s) from %ls -> %ls\n", g->count, argv[1], argv[2]);
    return 0;
}
