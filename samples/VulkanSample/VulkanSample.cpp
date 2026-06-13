// VulkanSample: minimal NV3DLib Vulkan consumer (inverted-export flow).
//
// The lib creates a DX11 NT-shared texture + fence on its bridge device and
// returns NT HANDLEs. The host (this sample) imports them as a VkImage +
// VkSemaphore (timeline) via the external_memory_win32 / external_semaphore_win32
// extensions. Each frame: CPU-fills a HOST_VISIBLE staging buffer with the
// test pattern, records vkCmdCopyBufferToImage to upload, submits with a
// timeline-semaphore signal, then calls NV3D::Present(value).

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "NV3D.hpp"

namespace {

constexpr uint32_t kWidth  = 2560;
constexpr uint32_t kHeight = 720;

void FillTestPattern(uint8_t* px, uint32_t pitch, uint32_t frame) {
    const uint32_t per_eye = kWidth / 2;
    const uint32_t cx = per_eye / 2;
    const uint32_t cy = kHeight / 2;
    const int disparity = static_cast<int>(20 + 15 * sinf(frame * 0.04f));
    const int quad_half = 40;
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = px + y * pitch;
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* p = row + x * 4;  // BGRA byte order — matches VK_FORMAT_B8G8R8A8_UNORM
            const bool right = (x >= per_eye);
            const uint32_t lx = right ? (x - per_eye) : x;
            if (!right) {
                p[0] = 0; p[1] = 0; p[2] = static_cast<uint8_t>(lx * 255 / per_eye); p[3] = 0xFF;
            } else {
                p[0] = 0; p[1] = static_cast<uint8_t>(lx * 255 / per_eye); p[2] = 0; p[3] = 0xFF;
            }
            int qx = static_cast<int>(lx) - static_cast<int>(cx);
            int qy = static_cast<int>(y) - static_cast<int>(cy);
            qx += right ? -disparity : disparity;
            if (qx >= -quad_half && qx <= quad_half &&
                qy >= -quad_half && qy <= quad_half) {
                p[0] = p[1] = p[2] = 0xFF;
            }
        }
    }
}

FILE* g_log_file = nullptr;
void LogSink(NV3D::LogLevel level, const wchar_t* msg, void*) {
    const wchar_t* lvl = L"";
    switch (level) {
        case NV3D::LogLevel::Debug:   lvl = L"D"; break;
        case NV3D::LogLevel::Info:    lvl = L"I"; break;
        case NV3D::LogLevel::Warning: lvl = L"W"; break;
        case NV3D::LogLevel::Error:   lvl = L"E"; break;
    }
    wprintf(L"[NV3D][%s] %s\n", lvl, msg);
    fflush(stdout);
    if (g_log_file) {
        fwprintf(g_log_file, L"[NV3D][%s] %s\n", lvl, msg);
        fflush(g_log_file);
    }
}

#define VK_CHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    wprintf(L#expr L" failed vr=%d\n", _r); return 1; } } while (0)

HMODULE g_vulkan_dll = nullptr;
PFN_vkGetInstanceProcAddr GetInstProcAddr = nullptr;

#define VK_LOAD_INSTANCE(inst, var, name) \
    var = reinterpret_cast<PFN_##name>(GetInstProcAddr(inst, #name)); \
    if (!var) { wprintf(L"missing " L#name L"\n"); return 1; }
#define VK_LOAD_DEVICE(getter, dev, var, name) \
    var = reinterpret_cast<PFN_##name>(getter(dev, #name)); \
    if (!var) { wprintf(L"missing " L#name L"\n"); return 1; }

}  // anonymous

int wmain() {
    _wfopen_s(&g_log_file, L"vulkansample.log", L"w, ccs=UTF-16LE");
    NV3D::SetLogSink(LogSink, nullptr);

    // -------- 1. Vulkan loader + instance --------
    g_vulkan_dll = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vulkan_dll) { wprintf(L"vulkan-1.dll not found\n"); return 1; }
    GetInstProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(g_vulkan_dll, "vkGetInstanceProcAddr"));
    if (!GetInstProcAddr) return 1;

    PFN_vkCreateInstance vkCreateInstance_;
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion_;
    VK_LOAD_INSTANCE(nullptr, vkCreateInstance_, vkCreateInstance);
    VK_LOAD_INSTANCE(nullptr, vkEnumerateInstanceVersion_, vkEnumerateInstanceVersion);

    VkApplicationInfo app{};
    app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "NV3DLib VulkanSample";
    app.apiVersion       = VK_API_VERSION_1_2;   // timeline semaphores in core
    const char* inst_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = sizeof(inst_exts)/sizeof(inst_exts[0]);
    ici.ppEnabledExtensionNames = inst_exts;
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance_(&ici, nullptr, &instance));

    // -------- 2. Physical device + queue family --------
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices_;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties_;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties_;
    PFN_vkCreateDevice vkCreateDevice_;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_;
    VK_LOAD_INSTANCE(instance, vkEnumeratePhysicalDevices_, vkEnumeratePhysicalDevices);
    VK_LOAD_INSTANCE(instance, vkGetPhysicalDeviceProperties_, vkGetPhysicalDeviceProperties);
    VK_LOAD_INSTANCE(instance, vkGetPhysicalDeviceQueueFamilyProperties_, vkGetPhysicalDeviceQueueFamilyProperties);
    VK_LOAD_INSTANCE(instance, vkCreateDevice_, vkCreateDevice);
    VK_LOAD_INSTANCE(instance, vkGetDeviceProcAddr_, vkGetDeviceProcAddr);
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties_;
    VK_LOAD_INSTANCE(instance, vkGetPhysicalDeviceMemoryProperties_, vkGetPhysicalDeviceMemoryProperties);

    uint32_t pd_count = 0;
    vkEnumeratePhysicalDevices_(instance, &pd_count, nullptr);
    std::vector<VkPhysicalDevice> pds(pd_count);
    vkEnumeratePhysicalDevices_(instance, &pd_count, pds.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (auto pd : pds) {
        VkPhysicalDeviceProperties pp{};
        vkGetPhysicalDeviceProperties_(pd, &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys = pd;
            wprintf(L"Picked %hs\n", pp.deviceName);
            break;
        }
    }
    if (!phys) { wprintf(L"no discrete GPU\n"); return 1; }

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties_(phys, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties_(phys, &qf_count, qfs.data());
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    }

    // -------- 3. Logical device with required extensions --------
    const char* dev_exts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };
    float qp = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = qfi;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &qp;
    VkPhysicalDeviceTimelineSemaphoreFeatures ts_feat{};
    ts_feat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
    ts_feat.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext                   = &ts_feat;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = sizeof(dev_exts)/sizeof(dev_exts[0]);
    dci.ppEnabledExtensionNames = dev_exts;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice_(phys, &dci, nullptr, &device));

    // Resolve device-level functions.
    PFN_vkGetDeviceQueue vkGetDeviceQueue_;
    PFN_vkCreateImage vkCreateImage_;
    PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements_;
    PFN_vkAllocateMemory vkAllocateMemory_;
    PFN_vkBindImageMemory vkBindImageMemory_;
    PFN_vkCreateBuffer vkCreateBuffer_;
    PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements_;
    PFN_vkBindBufferMemory vkBindBufferMemory_;
    PFN_vkMapMemory vkMapMemory_;
    PFN_vkCreateCommandPool vkCreateCommandPool_;
    PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers_;
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer_;
    PFN_vkEndCommandBuffer vkEndCommandBuffer_;
    PFN_vkResetCommandPool vkResetCommandPool_;
    PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier_;
    PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage_;
    PFN_vkQueueSubmit vkQueueSubmit_;
    PFN_vkCreateSemaphore vkCreateSemaphore_;
    PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR_;
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkGetDeviceQueue_, vkGetDeviceQueue);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCreateImage_, vkCreateImage);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkGetImageMemoryRequirements_, vkGetImageMemoryRequirements);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkAllocateMemory_, vkAllocateMemory);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkBindImageMemory_, vkBindImageMemory);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCreateBuffer_, vkCreateBuffer);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkGetBufferMemoryRequirements_, vkGetBufferMemoryRequirements);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkBindBufferMemory_, vkBindBufferMemory);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkMapMemory_, vkMapMemory);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCreateCommandPool_, vkCreateCommandPool);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkAllocateCommandBuffers_, vkAllocateCommandBuffers);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkBeginCommandBuffer_, vkBeginCommandBuffer);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkEndCommandBuffer_, vkEndCommandBuffer);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkResetCommandPool_, vkResetCommandPool);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCmdPipelineBarrier_, vkCmdPipelineBarrier);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCmdCopyBufferToImage_, vkCmdCopyBufferToImage);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkQueueSubmit_, vkQueueSubmit);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkCreateSemaphore_, vkCreateSemaphore);
    VK_LOAD_DEVICE(vkGetDeviceProcAddr_, device, vkImportSemaphoreWin32HandleKHR_, vkImportSemaphoreWin32HandleKHR);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue_(device, qfi, 0, &queue);

    auto findMemoryType = [&](uint32_t bits, VkMemoryPropertyFlags flags) -> uint32_t {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties_(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        return UINT32_MAX;
    };

    // -------- 4. NV3DLib init + import the shared resources --------
    NV3D::InitParams p{};
    p.enable_lightboost = true;
    NV3D::InterfaceVulkan* nv3d = nullptr;
    HRESULT hr = NV3D::CreateInterfaceVulkan(instance, phys, device, qfi, &p, &nv3d);
    if (FAILED(hr) || !nv3d) { wprintf(L"CreateInterfaceVulkan hr=0x%08X\n", hr); return 1; }

    HANDLE mem_nt = nullptr;
    HANDLE sem_nt = nullptr;
    hr = nv3d->InitSharedResources(kWidth, kHeight,
                                     87,   // DXGI_FORMAT_B8G8R8A8_UNORM
                                     &mem_nt, &sem_nt);
    if (FAILED(hr)) { wprintf(L"InitSharedResources hr=0x%08X\n", hr); return 1; }
    wprintf(L"lib gave us mem_nt=%p sem_nt=%p\n", mem_nt, sem_nt);

    // -------- 5. Create VkImage backed by imported memory --------
    VkExternalMemoryImageCreateInfo emi{};
    emi.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo iimg{};
    iimg.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iimg.pNext         = &emi;
    iimg.imageType     = VK_IMAGE_TYPE_2D;
    iimg.format        = VK_FORMAT_B8G8R8A8_UNORM;
    iimg.extent        = { kWidth, kHeight, 1 };
    iimg.mipLevels     = 1;
    iimg.arrayLayers   = 1;
    iimg.samples       = VK_SAMPLE_COUNT_1_BIT;
    iimg.tiling        = VK_IMAGE_TILING_OPTIMAL;
    iimg.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    iimg.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    iimg.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage sbs_img = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImage_(device, &iimg, nullptr, &sbs_img));

    VkMemoryRequirements mreq{};
    vkGetImageMemoryRequirements_(device, sbs_img, &mreq);
    VkImportMemoryWin32HandleInfoKHR imp{};
    imp.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    imp.handle     = mem_nt;
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image  = sbs_img;
    ded.pNext  = &imp;
    VkMemoryAllocateInfo mai{};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &ded;
    mai.allocationSize  = mreq.size;
    mai.memoryTypeIndex = findMemoryType(mreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory sbs_mem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory_(device, &mai, nullptr, &sbs_mem));
    VK_CHECK(vkBindImageMemory_(device, sbs_img, sbs_mem, 0));

    // -------- 6. Create + import the timeline semaphore --------
    VkSemaphoreTypeCreateInfo stc{};
    stc.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    stc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    stc.initialValue  = 0;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &stc;
    VkSemaphore sig_sem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSemaphore_(device, &sci, nullptr, &sig_sem));

    VkImportSemaphoreWin32HandleInfoKHR isimp{};
    isimp.sType      = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    isimp.semaphore  = sig_sem;
    isimp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    isimp.handle     = sem_nt;
    VK_CHECK(vkImportSemaphoreWin32HandleKHR_(device, &isimp));

    // -------- 7. CPU staging buffer + command pool --------
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer_(device, &bci, nullptr, &staging_buf));
    VkMemoryRequirements smr{};
    vkGetBufferMemoryRequirements_(device, staging_buf, &smr);
    VkMemoryAllocateInfo smi{};
    smi.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    smi.allocationSize  = smr.size;
    smi.memoryTypeIndex = findMemoryType(smr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateMemory_(device, &smi, nullptr, &staging_mem));
    VK_CHECK(vkBindBufferMemory_(device, staging_buf, staging_mem, 0));
    void* staging_ptr = nullptr;
    VK_CHECK(vkMapMemory_(device, staging_mem, 0, smr.size, 0, &staging_ptr));

    VkCommandPoolCreateInfo cpi{};
    cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = qfi;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool_(device, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers_(device, &cbai, &cmd));

    wprintf(L"VulkanSample: running. Ctrl+C to exit.\n");
    fflush(stdout);

    uint64_t sem_value = 0;
    auto next = std::chrono::steady_clock::now();
    for (uint32_t frame = 0; ; ++frame) {
        next += std::chrono::milliseconds(16);

        FillTestPattern(static_cast<uint8_t*>(staging_ptr), kWidth * 4, frame);

        vkResetCommandPool_(device, pool, 0);
        VkCommandBufferBeginInfo bbi{};
        bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer_(cmd, &bbi);

        VkImageMemoryBarrier b1{};
        b1.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b1.oldLayout        = (frame == 0) ? VK_IMAGE_LAYOUT_UNDEFINED
                                            : VK_IMAGE_LAYOUT_GENERAL;
        b1.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b1.srcAccessMask    = 0;
        b1.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b1.image                          = sbs_img;
        b1.subresourceRange.aspectMask    = VK_IMAGE_ASPECT_COLOR_BIT;
        b1.subresourceRange.levelCount    = 1;
        b1.subresourceRange.layerCount    = 1;
        vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &b1);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent                 = { kWidth, kHeight, 1 };
        vkCmdCopyBufferToImage_(cmd, staging_buf, sbs_img,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier b2 = b1;
        b2.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b2.dstAccessMask = 0;
        vkCmdPipelineBarrier_(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                0, nullptr, 0, nullptr, 1, &b2);
        vkEndCommandBuffer_(cmd);

        sem_value++;
        VkTimelineSemaphoreSubmitInfo tssi{};
        tssi.sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tssi.signalSemaphoreValueCount = 1;
        tssi.pSignalSemaphoreValues    = &sem_value;
        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.pNext                = &tssi;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &sig_sem;
        vkQueueSubmit_(queue, 1, &si, VK_NULL_HANDLE);

        HRESULT phr = nv3d->Present(sem_value);
        if (FAILED(phr)) { wprintf(L"Present hr=0x%08X\n", phr); break; }

        std::this_thread::sleep_until(next);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { nv3d->Delete(); return 0; }
        }
    }

    nv3d->Delete();
    return 0;
}
