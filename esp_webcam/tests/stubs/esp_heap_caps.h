#pragma once

#include <stddef.h>

#define MALLOC_CAP_SPIRAM 0u
#define MALLOC_CAP_8BIT 0u

void *heap_caps_malloc(size_t size, unsigned caps);
