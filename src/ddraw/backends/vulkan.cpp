// Vulkan 1.1 backend. vulkan-1.dll is loaded at runtime; shaders are the same
// GLSL as the OpenGL backend, compiled to SPIR-V at build time.
#define SWGBM_HAVE_VULKAN 1
#include "../renderer.h"
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vector>
#include <algorithm>
#include "shaders_vk.inc"

namespace swgbm {
namespace {

#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define VK_DEVICE(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdDraw) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore)

#define DECL(n) PFN_##n n = nullptr;
VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)
#undef DECL
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };

class VulkanRenderer : public Renderer {
public:
    const char* Name() const override { return "Vulkan"; }
    ~VulkanRenderer() override {
        if (dev_) {
            vkDeviceWaitIdle(dev_);
            DestroySwapchain();
            DestroyFrame();
            DestroyImg(pal_);
            DestroyImg(ov_);
            for (int i = 0; i < kInFlight; i++)
                if (ovStaging_[i]) { vkUnmapMemory(dev_, ovStagingMem_[i]); vkDestroyBuffer(dev_, ovStaging_[i], nullptr); vkFreeMemory(dev_, ovStagingMem_[i], nullptr); }
            if (fsOverlayModule_) vkDestroyShaderModule(dev_, fsOverlayModule_, nullptr);
            if (pipeOverlay_) vkDestroyPipeline(dev_, pipeOverlay_, nullptr);
            for (auto p : {pipePal_, pipeScale_}) if (p) vkDestroyPipeline(dev_, p, nullptr);
            for (auto l : {layoutPal_, layoutScale_}) if (l) vkDestroyPipelineLayout(dev_, l, nullptr);
            for (auto l : {dslPal_, dslScale_}) if (l) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
            if (pool_) vkDestroyDescriptorPool(dev_, pool_, nullptr);
            for (auto r : {rpPal_, rpScale_}) if (r) vkDestroyRenderPass(dev_, r, nullptr);
            for (auto s : {sampNearest_, sampLinear_}) if (s) vkDestroySampler(dev_, s, nullptr);
            for (int i = 0; i < kInFlight; i++) {
                if (fence_[i]) vkDestroyFence(dev_, fence_[i], nullptr);
                if (acquireSem_[i]) vkDestroySemaphore(dev_, acquireSem_[i], nullptr);
            }
            if (cmdPool_) vkDestroyCommandPool(dev_, cmdPool_, nullptr);
            if (vsModule_) vkDestroyShaderModule(dev_, vsModule_, nullptr);
            if (fsScaleModule_) vkDestroyShaderModule(dev_, fsScaleModule_, nullptr);
            vkDestroyDevice(dev_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
        if (inst_) vkDestroyInstance(inst_, nullptr);
    }

    bool Init(HWND hwnd) override {
        hwnd_ = hwnd;
        HMODULE m = LoadLibraryW(L"vulkan-1.dll");
        if (!m) return false;
        vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
        if (!vkGetInstanceProcAddr) return false;
#define LOADG(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n); if (!n) return false;
        VK_GLOBAL(LOADG)
#undef LOADG
        VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "SWGB Modern";
        app.apiVersion = VK_API_VERSION_1_1;
        const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
        VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = exts;
        if (vkCreateInstance(&ici, nullptr, &inst_) != VK_SUCCESS) { LOG("Vulkan: vkCreateInstance failed"); return false; }
#define LOADI(n) n = (PFN_##n)vkGetInstanceProcAddr(inst_, #n); if (!n) { LOG("Vulkan: missing " #n); return false; }
        VK_INSTANCE(LOADI)
#undef LOADI
        VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = hwnd;
        if (vkCreateWin32SurfaceKHR(inst_, &sci, nullptr, &surface_) != VK_SUCCESS) return false;

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst_, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(inst_, &n, devs.data());
        int bestScore = -1;
        for (auto d : devs) {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, qs.data());
            for (uint32_t i = 0; i < qn; i++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
                if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    VkPhysicalDeviceProperties pr;
                    vkGetPhysicalDeviceProperties(d, &pr);
                    int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
                    if (score > bestScore) { bestScore = score; phys_ = d; qfam_ = i; }
                    break;
                }
            }
        }
        if (!phys_) { LOG("Vulkan: no suitable device"); return false; }
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(phys_, &pr);
        LOG("Vulkan: %s (api %u.%u)", pr.deviceName, VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion));
        vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = qfam_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dexts;
        if (vkCreateDevice(phys_, &dci, nullptr, &dev_) != VK_SUCCESS) return false;
#define LOADD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev_, #n); if (!n) { LOG("Vulkan: missing " #n); return false; }
        VK_DEVICE(LOADD)
#undef LOADD
        vkGetDeviceQueue(dev_, qfam_, 0, &queue_);

        VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = qfam_;
        vkCreateCommandPool(dev_, &cpi, nullptr, &cmdPool_);
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmdPool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = kInFlight;
        vkAllocateCommandBuffers(dev_, &cai, cmdBufs_);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (int i = 0; i < kInFlight; i++) {
            vkCreateFence(dev_, &fci, nullptr, &fence_[i]);
            vkCreateSemaphore(dev_, &smi, nullptr, &acquireSem_[i]);
        }

        if (!CreateStatic()) return false;
        return CreateSwapchain(true);
    }

    bool Present(const Frame& f, const Viewport& vp, int cw, int ch, const PresentOptions& opt) override {
        if (cw <= 0 || ch <= 0) return true;
        cur_ = (cur_ + 1) % kInFlight;
        cmd_ = cmdBufs_[cur_];
        vkWaitForFences(dev_, 1, &fence_[cur_], VK_TRUE, UINT64_MAX);
        if (opt.vsync != vsync_ || (int)extent_.width != cw || (int)extent_.height != ch) {
            vsync_ = opt.vsync;
            vkDeviceWaitIdle(dev_);
            if (!CreateSwapchain(false)) return false;
        }
        if (f.width != fw_ || f.height != fh_) { vkDeviceWaitIdle(dev_); if (!CreateFrame(f.width, f.height)) return false; }

        uint32_t imageIndex = 0;
        VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, acquireSem_[cur_], VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(dev_); return CreateSwapchain(false); }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("Vulkan: acquire failed %d", r); return false; }
        vkResetFences(dev_, 1, &fence_[cur_]);

        // upload changed rows + palette through this frame's staging buffer
        int y0 = allDirty_ ? 0 : f.dirtyY0, y1 = allDirty_ ? f.height : f.dirtyY1;
        bool pal = allDirty_ || f.paletteChanged;
        uint8_t* p = stagingPtr_[cur_];
        if (y1 > y0) memcpy(p + (size_t)y0 * f.width, f.pixels.data() + (size_t)y0 * f.width, (size_t)(y1 - y0) * f.width);
        if (pal) memcpy(p + palOffset_, f.palette, 1024);

        vkResetCommandBuffer(cmd_, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &bi);
        auto upload = [&](Img& im, bool& valid, VkDeviceSize off, int ry0, int ry1, int w) {
            // keep previous contents (partial update) once the image is initialised
            Barrier(im.img, valid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, valid ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    valid ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c = {};
            c.bufferOffset = off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageOffset = {0, ry0, 0};
            c.imageExtent = {(uint32_t)w, (uint32_t)(ry1 - ry0), 1};
            vkCmdCopyBufferToImage(cmd_, stagingBuf_[cur_], im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            Barrier(im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            valid = true;
        };
        if (y1 > y0) upload(idx_, idxValid_, (VkDeviceSize)y0 * f.width, y0, y1, f.width);
        if (pal) upload(pal_, palValid_, palOffset_, 0, 1, 256);
        allDirty_ = false;
        bool drawOverlay = UploadOverlay();

        // pass 1: palette resolve into frame image
        VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rb.renderPass = rpPal_;
        rb.framebuffer = frameFb_;
        rb.renderArea.extent = {(uint32_t)f.width, (uint32_t)f.height};
        vkCmdBeginRenderPass(cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipePal_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutPal_, 0, 1, &setPal_, 0, nullptr);
        VkViewport v1 = {0, 0, (float)f.width, (float)f.height, 0, 1};
        VkRect2D s1 = {{0, 0}, {(uint32_t)f.width, (uint32_t)f.height}};
        vkCmdSetViewport(cmd_, 0, 1, &v1);
        vkCmdSetScissor(cmd_, 0, 1, &s1);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd_);

        // pass 2: scale into swapchain image
        VkClearValue clear = {};
        clear.color.float32[3] = 1.0f;
        rb.renderPass = rpScale_;
        rb.framebuffer = fbs_[imageIndex];
        rb.renderArea.extent = extent_;
        rb.clearValueCount = 1;
        rb.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeScale_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutScale_, 0, 1, &setScale_, 0, nullptr);
        VkViewport v2 = {(float)vp.x, (float)vp.y, (float)vp.w, (float)vp.h, 0, 1};
        VkRect2D s2 = {{vp.x, vp.y}, {(uint32_t)vp.w, (uint32_t)vp.h}};
        vkCmdSetViewport(cmd_, 0, 1, &v2);
        vkCmdSetScissor(cmd_, 0, 1, &s2);
        ScaleConstants sc = {};
        sc.srcSize[0] = (float)f.width; sc.srcSize[1] = (float)f.height; sc.srcSize[2] = 1.0f / f.width; sc.srcSize[3] = 1.0f / f.height;
        sc.dstSize[0] = (float)vp.w; sc.dstSize[1] = (float)vp.h; sc.dstSize[2] = 1.0f / vp.w; sc.dstSize[3] = 1.0f / vp.h;
        sc.filter = (int)opt.filter;
        sc.sharpness = opt.sharpness;
        vkCmdPushConstants(cmd_, layoutScale_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(sc), &sc);
        if (!(overlay_ && overlay_->visible && overlay_->hideFrame)) vkCmdDraw(cmd_, 3, 1, 0, 0);
        if (drawOverlay) DrawOverlay(f, vp);
        vkCmdEndRenderPass(cmd_);
        vkEndCommandBuffer(cmd_);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquireSem_[cur_];
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderSems_[imageIndex];
        if (vkQueueSubmit(queue_, 1, &si, fence_[cur_]) != VK_SUCCESS) { LOG("Vulkan: submit failed"); return false; }
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderSems_[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain_;
        pi.pImageIndices = &imageIndex;
        r = vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(dev_); return CreateSwapchain(false); }
        if (r == VK_ERROR_DEVICE_LOST) return false;
        return true;
    }

private:
    // Overlay upload (outside the render pass) - returns whether to draw it.
    bool UploadOverlay() {
        const Overlay* o = overlay_;
        if (!o || !o->visible || o->width <= 0 || o->height <= 0) return false;
        bool full = false;
        if (o->width != ovW_ || o->height != ovH_) {
            vkDeviceWaitIdle(dev_);
            DestroyImg(ov_);
            for (int i = 0; i < kInFlight; i++)
                if (ovStaging_[i]) { vkUnmapMemory(dev_, ovStagingMem_[i]); vkDestroyBuffer(dev_, ovStaging_[i], nullptr); vkFreeMemory(dev_, ovStagingMem_[i], nullptr); ovStaging_[i] = VK_NULL_HANDLE; }
            if (!CreateImg(ov_, o->width, o->height, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
            for (int i = 0; i < kInFlight; i++) {
                VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = (VkDeviceSize)o->width * o->height * 4;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                vkCreateBuffer(dev_, &bci, nullptr, &ovStaging_[i]);
                VkMemoryRequirements mr;
                vkGetBufferMemoryRequirements(dev_, ovStaging_[i], &mr);
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (vkAllocateMemory(dev_, &ai, nullptr, &ovStagingMem_[i]) != VK_SUCCESS) return false;
                vkBindBufferMemory(dev_, ovStaging_[i], ovStagingMem_[i], 0);
                vkMapMemory(dev_, ovStagingMem_[i], 0, VK_WHOLE_SIZE, 0, (void**)&ovPtr_[i]);
            }
            VkDescriptorImageInfo ii = {sampLinear_, ov_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet = ovSet_; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
            ovW_ = o->width; ovH_ = o->height; ovValid_ = false; full = true;
        }
        int y0 = full ? 0 : o->dirtyY0, y1 = full ? o->height : o->dirtyY1;
        if (y1 > y0) {
            size_t rowBytes = (size_t)o->width * 4;
            memcpy(ovPtr_[cur_] + (size_t)y0 * rowBytes, o->pixels.data() + (size_t)y0 * o->width, (size_t)(y1 - y0) * rowBytes);
            Barrier(ov_.img, ovValid_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    ovValid_ ? VK_ACCESS_SHADER_READ_BIT : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    ovValid_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c = {};
            c.bufferOffset = (VkDeviceSize)y0 * rowBytes;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageOffset = {0, y0, 0};
            c.imageExtent = {(uint32_t)o->width, (uint32_t)(y1 - y0), 1};
            vkCmdCopyBufferToImage(cmd_, ovStaging_[cur_], ov_.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            Barrier(ov_.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ovValid_ = true;
        }
        return ovValid_;
    }
    void DrawOverlay(const Frame& f, const Viewport& vp) {
        float x, y, w, h;
        OverlayRect(*overlay_, f, vp, x, y, w, h);
        VkViewport v = {x, y, w, h, 0, 1};
        VkRect2D sr = {{(int32_t)x, (int32_t)y}, {(uint32_t)(w + 0.5f), (uint32_t)(h + 0.5f)}};
        vkCmdSetViewport(cmd_, 0, 1, &v);
        vkCmdSetScissor(cmd_, 0, 1, &sr);
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeOverlay_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layoutScale_, 0, 1, &ovSet_, 0, nullptr);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
    }

    uint32_t MemType(uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }
    void Barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                 VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd_, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    bool CreateImg(Img& im, int w, int h, VkFormat fmt, VkImageUsageFlags usage) {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {(uint32_t)w, (uint32_t)h, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev_, &ii, nullptr, &im.img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev_, im.img, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(dev_, &ai, nullptr, &im.mem) != VK_SUCCESS) return false;
        vkBindImageMemory(dev_, im.img, im.mem, 0);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(dev_, &vi, nullptr, &im.view) == VK_SUCCESS;
    }
    void DestroyImg(Img& im) {
        if (im.view) vkDestroyImageView(dev_, im.view, nullptr);
        if (im.img) vkDestroyImage(dev_, im.img, nullptr);
        if (im.mem) vkFreeMemory(dev_, im.mem, nullptr);
        im = Img();
    }
    VkShaderModule Module(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &ci, nullptr, &m);
        return m;
    }
    VkRenderPass RenderPass(VkFormat fmt, bool toPresent) {
        VkAttachmentDescription a = {};
        a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = toPresent ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = toPresent ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp = {};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1; ci.pAttachments = &a;
        ci.subpassCount = 1; ci.pSubpasses = &sp;
        ci.dependencyCount = 2; ci.pDependencies = dep;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(dev_, &ci, nullptr, &rp);
        return rp;
    }
    VkPipeline Pipeline(VkRenderPass rp, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, bool blend = false) {
        VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba = {};
        cba.colorWriteMask = 0xF;
        if (blend) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia;
        pi.pViewportState = &vps; pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb;
        pi.pDynamicState = &ds; pi.layout = layout; pi.renderPass = rp;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
        return p;
    }
    bool CreateStatic() {
        VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(dev_, &sci, nullptr, &sampNearest_);
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        vkCreateSampler(dev_, &sci, nullptr, &sampLinear_);

        VkDescriptorSetLayoutBinding b[2] = {};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1] = b[0]; b[1].binding = 1;
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 2; dl.pBindings = b;
        vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &dslPal_);
        dl.bindingCount = 1;
        vkCreateDescriptorSetLayout(dev_, &dl, nullptr, &dslScale_);
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &dslPal_;
        vkCreatePipelineLayout(dev_, &pl, nullptr, &layoutPal_);
        VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScaleConstants)};
        pl.pSetLayouts = &dslScale_;
        pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(dev_, &pl, nullptr, &layoutScale_);
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
        VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 3; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
        vkCreateDescriptorPool(dev_, &dp, nullptr, &pool_);
        VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = pool_; da.descriptorSetCount = 1; da.pSetLayouts = &dslPal_;
        vkAllocateDescriptorSets(dev_, &da, &setPal_);
        da.pSetLayouts = &dslScale_;
        vkAllocateDescriptorSets(dev_, &da, &setScale_);

        rpPal_ = RenderPass(VK_FORMAT_R8G8B8A8_UNORM, false);
        VkShaderModule vs = Module(kVkVertex, sizeof(kVkVertex)), fp = Module(kVkPalette, sizeof(kVkPalette)), fsc = Module(kVkScale, sizeof(kVkScale));
        pipePal_ = Pipeline(rpPal_, layoutPal_, vs, fp);
        vsModule_ = vs; fsScaleModule_ = fsc;
        fsOverlayModule_ = Module(kVkOverlay, sizeof(kVkOverlay));
        {
            VkDescriptorSetAllocateInfo oa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            oa.descriptorPool = pool_; oa.descriptorSetCount = 1; oa.pSetLayouts = &dslScale_;
            vkAllocateDescriptorSets(dev_, &oa, &ovSet_);
        }
        vkDestroyShaderModule(dev_, fp, nullptr);
        if (!pipePal_) return false;
        if (!CreateImg(pal_, 256, 1, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
        return true;
    }
    void DestroyFrame() {
        if (frameFb_) { vkDestroyFramebuffer(dev_, frameFb_, nullptr); frameFb_ = VK_NULL_HANDLE; }
        DestroyImg(idx_);
        DestroyImg(frame_);
        for (int i = 0; i < kInFlight; i++) {
            if (stagingBuf_[i]) { vkUnmapMemory(dev_, stagingMem_[i]); vkDestroyBuffer(dev_, stagingBuf_[i], nullptr); vkFreeMemory(dev_, stagingMem_[i], nullptr); }
            stagingBuf_[i] = VK_NULL_HANDLE; stagingPtr_[i] = nullptr;
        }
        idxValid_ = false;
    }
    bool CreateFrame(int w, int h) {
        DestroyFrame();
        if (!CreateImg(idx_, w, h, VK_FORMAT_R8_UINT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
        if (!CreateImg(frame_, w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rpPal_; fi.attachmentCount = 1; fi.pAttachments = &frame_.view;
        fi.width = w; fi.height = h; fi.layers = 1;
        if (vkCreateFramebuffer(dev_, &fi, nullptr, &frameFb_) != VK_SUCCESS) return false;
        palOffset_ = ((VkDeviceSize)w * h + 255) & ~(VkDeviceSize)255;
        for (int i = 0; i < kInFlight; i++) {
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = palOffset_ + 1024;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCreateBuffer(dev_, &bci, nullptr, &stagingBuf_[i]);
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(dev_, stagingBuf_[i], &mr);
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(dev_, &ai, nullptr, &stagingMem_[i]) != VK_SUCCESS) return false;
            vkBindBufferMemory(dev_, stagingBuf_[i], stagingMem_[i], 0);
            vkMapMemory(dev_, stagingMem_[i], 0, VK_WHOLE_SIZE, 0, (void**)&stagingPtr_[i]);
        }
        allDirty_ = true;
        VkDescriptorImageInfo ii[2] = {{sampNearest_, idx_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                      {sampNearest_, pal_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkDescriptorImageInfo fi2 = {sampLinear_, frame_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        wr[0].dstSet = setPal_; wr[0].descriptorCount = 2; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[0].pImageInfo = ii;
        wr[1].dstSet = setScale_; wr[1].descriptorCount = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[1].pImageInfo = &fi2;
        vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
        fw_ = w; fh_ = h;
        return true;
    }
    void DestroySwapchain() {
        for (auto fb : fbs_) vkDestroyFramebuffer(dev_, fb, nullptr);
        for (auto v : views_) vkDestroyImageView(dev_, v, nullptr);
        for (auto s : renderSems_) vkDestroySemaphore(dev_, s, nullptr);
        fbs_.clear(); views_.clear(); renderSems_.clear();
        if (pipeScale_) { vkDestroyPipeline(dev_, pipeScale_, nullptr); pipeScale_ = VK_NULL_HANDLE; }
        if (pipeOverlay_) { vkDestroyPipeline(dev_, pipeOverlay_, nullptr); pipeOverlay_ = VK_NULL_HANDLE; }
        if (rpScale_) { vkDestroyRenderPass(dev_, rpScale_, nullptr); rpScale_ = VK_NULL_HANDLE; }
        if (swapchain_) { vkDestroySwapchainKHR(dev_, swapchain_, nullptr); swapchain_ = VK_NULL_HANDLE; }
    }
    bool CreateSwapchain(bool first) {
        (void)first;
        DestroySwapchain();
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
        RECT rc;
        GetClientRect(hwnd_, &rc);
        extent_ = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{(uint32_t)rc.right, (uint32_t)rc.bottom};
        if (!extent_.width || !extent_.height) return true;
        uint32_t nf = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(nf);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, fmts.data());
        VkSurfaceFormatKHR fmt = fmts[0];
        for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) fmt = f;
        uint32_t npm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &npm, nullptr);
        std::vector<VkPresentModeKHR> pms(npm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &npm, pms.data());
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        if (!vsync_) for (auto pm : pms) if (pm == VK_PRESENT_MODE_IMMEDIATE_KHR || (pm == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) mode = pm;
        VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface_;
        ci.minImageCount = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
        ci.imageFormat = fmt.format; ci.imageColorSpace = fmt.colorSpace; ci.imageExtent = extent_;
        ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; ci.presentMode = mode; ci.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(dev_, &ci, nullptr, &swapchain_) != VK_SUCCESS) { LOG("Vulkan: swapchain creation failed"); return false; }
        uint32_t ni = 0;
        vkGetSwapchainImagesKHR(dev_, swapchain_, &ni, nullptr);
        std::vector<VkImage> imgs(ni);
        vkGetSwapchainImagesKHR(dev_, swapchain_, &ni, imgs.data());
        rpScale_ = RenderPass(fmt.format, true);
        pipeScale_ = Pipeline(rpScale_, layoutScale_, vsModule_, fsScaleModule_);
        pipeOverlay_ = Pipeline(rpScale_, layoutScale_, vsModule_, fsOverlayModule_, true);
        VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto im : imgs) {
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v;
            vkCreateImageView(dev_, &vi, nullptr, &v);
            views_.push_back(v);
            VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = rpScale_; fi.attachmentCount = 1; fi.pAttachments = &v;
            fi.width = extent_.width; fi.height = extent_.height; fi.layers = 1;
            VkFramebuffer fb;
            vkCreateFramebuffer(dev_, &fi, nullptr, &fb);
            fbs_.push_back(fb);
            VkSemaphore s;
            vkCreateSemaphore(dev_, &smi, nullptr, &s);
            renderSems_.push_back(s);
        }
        LOG("Vulkan: swapchain %ux%u, %u images, present mode %d", extent_.width, extent_.height, ni, (int)mode);
        return pipeScale_ != VK_NULL_HANDLE;
    }

    HWND hwnd_ = nullptr;
    VkInstance inst_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_ = {};
    uint32_t qfam_ = 0;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    static const int kInFlight = 2;
    VkCommandBuffer cmdBufs_[kInFlight] = {};
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_[kInFlight] = {};
    VkSemaphore acquireSem_[kInFlight] = {};
    int cur_ = 0;
    bool idxValid_ = false, palValid_ = false, allDirty_ = true;
    std::vector<VkSemaphore> renderSems_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_ = {0, 0};
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> fbs_;
    VkSampler sampNearest_ = VK_NULL_HANDLE, sampLinear_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslPal_ = VK_NULL_HANDLE, dslScale_ = VK_NULL_HANDLE;
    VkPipelineLayout layoutPal_ = VK_NULL_HANDLE, layoutScale_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet setPal_ = VK_NULL_HANDLE, setScale_ = VK_NULL_HANDLE;
    VkRenderPass rpPal_ = VK_NULL_HANDLE, rpScale_ = VK_NULL_HANDLE;
    VkPipeline pipePal_ = VK_NULL_HANDLE, pipeScale_ = VK_NULL_HANDLE;
    VkShaderModule vsModule_ = VK_NULL_HANDLE, fsScaleModule_ = VK_NULL_HANDLE;
    Img idx_, pal_, frame_, ov_;
    VkPipeline pipeOverlay_ = VK_NULL_HANDLE;
    VkShaderModule fsOverlayModule_ = VK_NULL_HANDLE;
    VkDescriptorSet ovSet_ = VK_NULL_HANDLE;
    VkBuffer ovStaging_[2] = {};
    VkDeviceMemory ovStagingMem_[2] = {};
    uint8_t* ovPtr_[2] = {};
    int ovW_ = 0, ovH_ = 0;
    bool ovValid_ = false;
    VkFramebuffer frameFb_ = VK_NULL_HANDLE;
    VkBuffer stagingBuf_[kInFlight] = {};
    VkDeviceMemory stagingMem_[kInFlight] = {};
    uint8_t* stagingPtr_[kInFlight] = {};
    VkDeviceSize palOffset_ = 0;
    int fw_ = 0, fh_ = 0;
    bool vsync_ = true;
};

} // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer() { return std::make_unique<VulkanRenderer>(); }

} // namespace swgbm
