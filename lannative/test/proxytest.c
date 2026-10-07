// Smoke test for the version.dll proxy: run from a folder that contains our version.dll. Not the game, so the DLL must
// (1) load, (2) forward the real version.dll functions correctly, (3) skip hooking ("not the game executable") and write its log.
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "version.lib")

int main(void) {
    HMODULE m = GetModuleHandleW(L"version.dll");
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(m, path, MAX_PATH);
    printf("version.dll loaded from: %ls\n", path);

    // real work through the forwarded exports: read this exe's own version info (none is embedded -> size 0 is fine),
    // and read kernel32's, which has one.
    DWORD h = 0;
    DWORD size = GetFileVersionInfoSizeW(L"C:\\Windows\\System32\\kernel32.dll", &h);
    printf("GetFileVersionInfoSizeW(kernel32) = %lu\n", size);
    if (size) {
        BYTE *buf = (BYTE *)malloc(size);
        if (GetFileVersionInfoW(L"C:\\Windows\\System32\\kernel32.dll", 0, size, buf)) {
            VS_FIXEDFILEINFO *ffi = NULL; UINT len = 0;
            if (VerQueryValueW(buf, L"\\", (LPVOID *)&ffi, &len) && ffi)
                printf("kernel32 version %u.%u.%u.%u\n", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                       HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        }
        free(buf);
    }
    return size ? 0 : 1;
}
