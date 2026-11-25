// Used only when building without the Vulkan SDK.
#include "../renderer.h"
namespace swgbm { std::unique_ptr<Renderer> CreateVulkanRenderer() { return nullptr; } }
