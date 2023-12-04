#include <gpu/vulcano/vram_allocator.h>

namespace cosmic::gpu::vulcano {
    VramManager::VramManager(raw_reference<GraphicsLayer> gpu) : graphics(gpu) {
        VmaAllocatorCreateInfo allocatorInfo{};
        vmaCreateAllocator(&allocatorInfo, &vma);
    }
    VramManager::~VramManager() {
        if (vma != VK_NULL_HANDLE)
            vmaDestroyAllocator(vma);
    }
}
