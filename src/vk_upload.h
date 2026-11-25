// Uploads the software-rendered tab into Vulkan images for SetOverlayTexture.
//
// SetOverlayRaw makes SteamVR create a new texture on every call, and the dashboard flickers while
// it swaps them. Handing it our own images instead (three, used in turn, so the one SteamVR is
// still showing is never overwritten) gives steady updates.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lh {

class VkUpload {
public:
    struct Image {   // what OpenVR's VRVulkanTextureData_t needs
        VkImage image;
        VkDevice device;
        VkPhysicalDevice physical;
        VkInstance instance;
        VkQueue queue;
        uint32_t family, width, height;
        VkFormat format;
    };
    // Lets the caller (OpenVR) choose extensions and the GPU; tests pass plain defaults.
    struct Hooks {
        std::vector<std::string> instance_extensions;
        std::function<VkPhysicalDevice(VkInstance)> pick_device;              // may return null
        std::function<std::vector<std::string>(VkPhysicalDevice)> device_extensions;
    };

    ~VkUpload() { release(); }
    bool init(uint32_t w, uint32_t h, const Hooks &hooks, std::string *err);
    bool ready() const { return dev_ != VK_NULL_HANDLE; }
    // Copies w*h RGBA pixels into the next image, waits for the copy, and returns that image,
    // left in TRANSFER_SRC_OPTIMAL as OpenVR expects.
    bool upload(const uint8_t *rgba, Image *out, std::string *err);
    // Reads an image back (for tests).
    bool read_back(const Image &img, std::vector<uint8_t> *rgba, std::string *err);
    void release();

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_mem = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_mem = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool used = false;
    };
    static constexpr int kSlots = 3;
    bool memory_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) const;

    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Slot slots_[kSlots];
    int next_ = 0;
    uint32_t w_ = 0, h_ = 0;
};

}  // namespace lh
