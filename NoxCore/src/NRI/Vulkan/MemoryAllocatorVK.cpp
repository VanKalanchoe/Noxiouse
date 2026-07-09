#include "MemoryAllocatorVK.h"

#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#include "vk_mem_alloc.hpp"
#include "vk_mem_alloc_raii.hpp"