#ifndef LANNATIVE_OBJECTSCAN_H
#define LANNATIVE_OBJECTSCAN_H

#include <stdint.h>

// Matches the validity mask used by this build's FWeakObjectPtr::Get.
static int object_item_is_live(uint32_t internal_flags) { return (internal_flags & 0x30000000u) == 0; }

#endif
