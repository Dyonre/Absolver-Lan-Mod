// seread.h - se_readable(p, n): 1 if all n bytes at p are committed, readable memory (VirtualQuery, no access). Used by call_function_params (secore.inc)
// to refuse a garbage argument pointer before copying from it. Spans region boundaries.
#pragma once
#include <windows.h>
#include <stdint.h>
static int se_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || (uintptr_t)p <= 0x10000 || (uintptr_t)p >= 0x00007FFFFFFFFFFFull || !VirtualQuery(p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mi.Protect) return 0;
    uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    return (uintptr_t)p + n <= end || se_readable((const void *)end, (uintptr_t)p + n - end);
}
