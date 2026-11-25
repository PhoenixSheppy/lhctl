#include "vk_upload.h"

#include <cstring>

namespace lh {

namespace {

#define TRY(call, what)                                                                                                \
    do {                                                                                                               \
        VkResult r_ = (call);                                                                                          \
        if (r_ != VK_SUCCESS) {                                                                                        \
            if (err)                                                                                                   \
                *err = std::string(what) + " failed (" + std::to_string(int(r_)) + ")";                               \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

std::vector<const char *> c_strs(const std::vector<std::string> &v) {
    std::vector<const char *> out;
    for (const auto &s : v)
        out.push_back(s.c_str());
    return out;
}

void layout(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
            VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

}  // namespace

bool VkUpload::memory_type(uint32_t bits, VkMemoryPropertyFlags want, uint32_t *out) const {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(phys_, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return true;
        }
    return false;
}

bool VkUpload::init(uint32_t w, uint32_t h, const Hooks &hooks, std::string *err) {
    release();
    w_ = w;
    h_ = h;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "lhctl";
    app.apiVersion = VK_API_VERSION_1_1;
    const auto iext = c_strs(hooks.instance_extensions);
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(iext.size());
    ici.ppEnabledExtensionNames = iext.data();
    TRY(vkCreateInstance(&ici, nullptr, &inst_), "vkCreateInstance");

    if (hooks.pick_device)
        phys_ = hooks.pick_device(inst_);
    if (!phys_) {
        uint32_t n = 1;
        VkResult r = vkEnumeratePhysicalDevices(inst_, &n, &phys_);
        if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !phys_) {
            if (err)
                *err = "no Vulkan device";
            release();
            return false;
        }
    }

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &nq, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < nq && !found; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family_ = i;
            found = true;
        }
    if (!found) {
        if (err)
            *err = "no graphics queue";
        release();
        return false;
    }

    const auto dext_s = hooks.device_extensions ? hooks.device_extensions(phys_) : std::vector<std::string>{};
    const auto dext = c_strs(dext_s);
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dext.size());
    dci.ppEnabledExtensionNames = dext.data();
    if (vkCreateDevice(phys_, &dci, nullptr, &dev_) != VK_SUCCESS) {
        dev_ = VK_NULL_HANDLE;
        if (err)
            *err = "vkCreateDevice failed";
        release();
        return false;
    }
    vkGetDeviceQueue(dev_, family_, 0, &queue_);

    auto fail = [&](const char *what) {
        if (err)
            *err = what;
        release();
        return false;
    };

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    if (vkCreateCommandPool(dev_, &pci, nullptr, &pool_) != VK_SUCCESS)
        return fail("vkCreateCommandPool failed");

    const VkDeviceSize bytes = VkDeviceSize(w_) * h_ * 4;
    for (Slot &s : slots_) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {w_, h_, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev_, &ii, nullptr, &s.image) != VK_SUCCESS)
            return fail("vkCreateImage failed");
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev_, s.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        if (!memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ai.memoryTypeIndex) ||
            vkAllocateMemory(dev_, &ai, nullptr, &s.image_mem) != VK_SUCCESS ||
            vkBindImageMemory(dev_, s.image, s.image_mem, 0) != VK_SUCCESS)
            return fail("image memory");

        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(dev_, &bi, nullptr, &s.staging) != VK_SUCCESS)
            return fail("vkCreateBuffer failed");
        vkGetBufferMemoryRequirements(dev_, s.staging, &mr);
        ai.allocationSize = mr.size;
        if (!memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &ai.memoryTypeIndex) ||
            vkAllocateMemory(dev_, &ai, nullptr, &s.staging_mem) != VK_SUCCESS ||
            vkBindBufferMemory(dev_, s.staging, s.staging_mem, 0) != VK_SUCCESS ||
            vkMapMemory(dev_, s.staging_mem, 0, bytes, 0, &s.mapped) != VK_SUCCESS)
            return fail("staging memory");

        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev_, &cai, &s.cmd) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers failed");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(dev_, &fci, nullptr, &s.fence) != VK_SUCCESS)
            return fail("vkCreateFence failed");
    }
    return true;
}

bool VkUpload::upload(const uint8_t *rgba, Image *out, std::string *err) {
    if (!ready()) {
        if (err)
            *err = "not initialized";
        return false;
    }
    Slot &s = slots_[next_];
    next_ = (next_ + 1) % kSlots;
    TRY(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    TRY(vkResetFences(dev_, 1, &s.fence), "vkResetFences");
    memcpy(s.mapped, rgba, size_t(w_) * h_ * 4);

    VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    TRY(vkBeginCommandBuffer(s.cmd, &bb), "vkBeginCommandBuffer");
    layout(s.cmd, s.image, s.used ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
           VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w_, h_, 1};
    vkCmdCopyBufferToImage(s.cmd, s.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    layout(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    TRY(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    TRY(vkQueueSubmit(queue_, 1, &si, s.fence), "vkQueueSubmit");
    // Finish the copy before SteamVR reads the image.
    TRY(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    s.used = true;

    *out = Image{s.image, dev_, phys_, inst_, queue_, family_, w_, h_, VK_FORMAT_R8G8B8A8_UNORM};
    return true;
}

bool VkUpload::read_back(const Image &img, std::vector<uint8_t> *rgba, std::string *err) {
    // Reuse the slot that owns the image: copy image -> its staging buffer, then read it.
    for (Slot &s : slots_) {
        if (s.image != img.image)
            continue;
        TRY(vkResetFences(dev_, 1, &s.fence), "vkResetFences");
        VkCommandBufferBeginInfo bb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        TRY(vkBeginCommandBuffer(s.cmd, &bb), "vkBeginCommandBuffer");
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {w_, h_, 1};
        vkCmdCopyImageToBuffer(s.cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.staging, 1, &c);
        TRY(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s.cmd;
        TRY(vkQueueSubmit(queue_, 1, &si, s.fence), "vkQueueSubmit");
        TRY(vkWaitForFences(dev_, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        const auto *p = static_cast<const uint8_t *>(s.mapped);
        rgba->assign(p, p + size_t(w_) * h_ * 4);
        return true;
    }
    if (err)
        *err = "unknown image";
    return false;
}

void VkUpload::release() {
    if (dev_) {
        vkDeviceWaitIdle(dev_);
        for (Slot &s : slots_) {
            if (s.fence)
                vkDestroyFence(dev_, s.fence, nullptr);
            if (s.staging_mem) {
                if (s.mapped)
                    vkUnmapMemory(dev_, s.staging_mem);
                vkFreeMemory(dev_, s.staging_mem, nullptr);
            }
            if (s.staging)
                vkDestroyBuffer(dev_, s.staging, nullptr);
            if (s.image)
                vkDestroyImage(dev_, s.image, nullptr);
            if (s.image_mem)
                vkFreeMemory(dev_, s.image_mem, nullptr);
            s = Slot{};
        }
        if (pool_)
            vkDestroyCommandPool(dev_, pool_, nullptr);
        vkDestroyDevice(dev_, nullptr);
    }
    if (inst_)
        vkDestroyInstance(inst_, nullptr);
    pool_ = VK_NULL_HANDLE;
    dev_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    phys_ = VK_NULL_HANDLE;
    inst_ = VK_NULL_HANDLE;
    next_ = 0;
}

}  // namespace lh
