#include "../renderer.h"
namespace swgbm {
#ifndef SWGBM_HAVE_VULKAN
std::unique_ptr<Renderer> CreateVulkanRenderer() { return nullptr; }
#endif
}
