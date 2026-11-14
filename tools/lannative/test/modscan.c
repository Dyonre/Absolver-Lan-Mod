// modscan <ModsFolder> : offline conflict report using the same code as the in-game loader (read-only; never touches the game).
// Order = folder names sorted (loadorder.txt is not read). Usage: modscan.exe C:\path\to\Mods
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../modconf.h"

static int cmp(const void *a, const void *b) { return _stricmp(*(char *const *)a, *(char *const *)b); }
int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: modscan <Mods folder>"); return 2; }
    char pat[1024]; snprintf(pat, sizeof pat, "%s\\*", argv[1]); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { puts("cannot open folder"); return 2; }
    char *mods[MODS_MAX]; int nm = 0;
    do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && nm < MODS_MAX) mods[nm++] = _strdup(fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h); qsort(mods, nm, sizeof *mods, cmp);
    static PakEntry *lists[MODS_MAX]; static ModFiles sets[MODS_MAX]; int ns = 0;
    for (int m = 0; m < nm; m++) {
        snprintf(pat, sizeof pat, "%s\\%s\\*.pak", argv[1], mods[m]); h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            char path[1200]; snprintf(path, sizeof path, "%s\\%s\\%s", argv[1], mods[m], fd.cFileName);
            FILE *f = fopen(path, "rb"); unsigned char ih[20]; PakEntry *e = NULL; int n = f ? modpak_read(f, &e, ih) : -2; if (f) fclose(f);
            printf("%-28s %-44s %7d files  indexhash %02X%02X%02X%02X..\n", mods[m], fd.cFileName, n, ih[0], ih[1], ih[2], ih[3]);
            if (n > 0 && ns < MODS_MAX) { lists[ns] = e; sets[ns].entries = e; sets[ns].count = n; sets[ns].mod = m; ns++; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    static Conflict cf[4096]; int nc = mods_conflicts(sets, ns, cf, 4096); if (nc > 4096) nc = 4096;
    printf("\n%d file(s) shipped by more than one mod\n", nc);
    int pairs[MODS_MAX][MODS_MAX] = {{0}}, same[MODS_MAX][MODS_MAX] = {{0}};
    for (int i = 0; i < nc; i++) { pairs[cf[i].loser][cf[i].winner]++; if (cf[i].same_content) same[cf[i].loser][cf[i].winner]++; }
    for (int a = 0; a < nm; a++) for (int b = 0; b < nm; b++) if (pairs[a][b]) printf("  %s is overridden by %s on %d file(s) (%d identical)\n", mods[a], mods[b], pairs[a][b], same[a][b]);
    int shown = 0; for (int i = 0; i < nc && shown < 40; i++) if (!cf[i].same_content) { printf("    %s  [%s -> %s]\n", cf[i].path, mods[cf[i].loser], mods[cf[i].winner]); shown++; }
    static SplitAsset sp[256]; int nsp = mods_split_assets(sets, ns, sp, 256); if (nsp > 256) nsp = 256;
    printf("\n%d split asset(s) (parts served by different mods: likely crash)\n", nsp);
    for (int i = 0; i < nsp && i < 40; i++) { printf("    %s :", sp[i].base); for (int k = 0; k < sp[i].nmods; k++) printf(" %s", mods[sp[i].mods[k]]); printf("\n"); }
    return 0;
}
