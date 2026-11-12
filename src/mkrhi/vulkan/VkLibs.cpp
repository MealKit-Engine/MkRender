// External library implementations (warnings off, engine/CMakeLists.txt): volk (loader), VMA (memory allocation)
#include "Vk.h"

#include "volk.c"

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
