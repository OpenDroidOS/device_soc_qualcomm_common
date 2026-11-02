/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcom/vulkan_buffer_allocator.h"
#include "qcom/native_buffer_export.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <display_type.h>

#include <dlfcn.h>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <new>
#include <unistd.h>

namespace OHOS::Qcom {
namespace {
int Error(VkResult result) noexcept
{
    switch (result) {
        case VK_SUCCESS: return 0;
        case VK_ERROR_OUT_OF_HOST_MEMORY: return -ENOMEM;
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return -ENOSPC;
        case VK_ERROR_DEVICE_LOST: return -ENODEV;
        case VK_ERROR_FORMAT_NOT_SUPPORTED:
        case VK_ERROR_FEATURE_NOT_PRESENT:
        case VK_ERROR_EXTENSION_NOT_PRESENT:
        case VK_ERROR_INCOMPATIBLE_DRIVER: return -ENOTSUP;
        case VK_ERROR_TOO_MANY_OBJECTS: return -EMFILE;
        case VK_INCOMPLETE: return -EAGAIN;
        default: return -EIO;
    }
}

struct Format {
    VkFormat vk;
    uint32_t planes;
};

Format NativeFormat(int32_t format) noexcept
{
    switch (format) {
        case NATIVEBUFFER_PIXEL_FMT_RGBA_8888: return {VK_FORMAT_R8G8B8A8_UNORM, 1};
        case NATIVEBUFFER_PIXEL_FMT_BGRA_8888: return {VK_FORMAT_B8G8R8A8_UNORM, 1};
        case NATIVEBUFFER_PIXEL_FMT_RGB_565: return {VK_FORMAT_R5G6B5_UNORM_PACK16, 1};
        case NATIVEBUFFER_PIXEL_FMT_BGR_565: return {VK_FORMAT_B5G6R5_UNORM_PACK16, 1};
        case NATIVEBUFFER_PIXEL_FMT_RGB_888: return {VK_FORMAT_R8G8B8_UNORM, 1};
        case NATIVEBUFFER_PIXEL_FMT_RGBA_1010102: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1};
        case NATIVEBUFFER_PIXEL_FMT_RGBA16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 1};
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP: return {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 2};
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_P: return {VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, 3};
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_P010:
            return {VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, 2};
        default: return {VK_FORMAT_UNDEFINED, 0};
    }
}

bool Extension(const VkExtensionProperties* properties, uint32_t count, const char* name) noexcept
{
    for (uint32_t i = 0; i < count; i++) {
        if (std::strcmp(properties[i].extensionName, name) == 0)
            return true;
    }
    return false;
}
} // namespace

struct VulkanBufferAllocator::Impl {
    void* library = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    bool ycbcr = false;
    mutable std::mutex mutex;
    VkPhysicalDeviceMemoryProperties memoryProperties {};
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 getImageProperties = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getRequirements = nullptr;
    PFN_vkGetImageSubresourceLayout getLayout = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindMemory = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;

    ~Impl()
    {
        if (device != VK_NULL_HANDLE)
            destroyDevice(device, nullptr);
        if (instance != VK_NULL_HANDLE)
            destroyInstance(instance, nullptr);
        if (library != nullptr)
            dlclose(library);
    }

    struct Image {
        const Impl& owner;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        ~Image()
        {
            if (image != VK_NULL_HANDLE)
                owner.destroyImage(owner.device, image, nullptr);
            if (memory != VK_NULL_HANDLE)
                owner.freeMemory(owner.device, memory, nullptr);
        }
    };

    int Open(const char* loader, const uint8_t* uuid) noexcept;
    int Prepare(const NativeBufferRequest& request, Image& image, Format& format,
        VkMemoryRequirements& requirements, uint32_t& memoryType) const noexcept;
};

int VulkanBufferAllocator::Impl::Open(const char* loader, const uint8_t* uuid) noexcept
{
    library = dlopen(loader, RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
        return -ENOENT;
    getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
    destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(library, "vkDestroyInstance"));
    if (getInstanceProcAddr == nullptr || destroyInstance == nullptr)
        return -EPROTO;
    auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        getInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    uint32_t version = 0;
    if (enumerateVersion == nullptr || createInstance == nullptr)
        return -ENOTSUP;
    VkResult status = enumerateVersion(&version);
    if (status != VK_SUCCESS)
        return Error(status);
    if (version < VK_API_VERSION_1_2)
        return -ENOTSUP;
    VkApplicationInfo application {};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instanceInfo {};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &application;
    VkInstance created = VK_NULL_HANDLE;
    status = createInstance(&instanceInfo, nullptr, &created);
    if (status != VK_SUCCESS)
        return Error(status);
    instance = created;

#define INSTANCE_PROC(type, name) auto name = reinterpret_cast<PFN_##type>(getInstanceProcAddr(instance, #type))
    INSTANCE_PROC(vkEnumeratePhysicalDevices, enumerateDevices);
    INSTANCE_PROC(vkGetPhysicalDeviceProperties2, getProperties);
    INSTANCE_PROC(vkGetPhysicalDeviceFeatures2, getFeatures);
    INSTANCE_PROC(vkEnumerateDeviceExtensionProperties, enumerateExtensions);
    INSTANCE_PROC(vkGetPhysicalDeviceQueueFamilyProperties, getQueues);
    INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties, getMemoryProperties);
    INSTANCE_PROC(vkCreateDevice, createDevice);
#undef INSTANCE_PROC
    getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(getInstanceProcAddr(instance, "vkDestroyDevice"));
    getImageProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        getInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (!enumerateDevices || !getProperties || !getFeatures || !enumerateExtensions || !getQueues ||
        !getMemoryProperties || !createDevice || !getDeviceProcAddr || !destroyDevice || !getImageProperties)
        return -EPROTO;
    uint32_t count = 0;
    status = enumerateDevices(instance, &count, nullptr);
    if (status != VK_SUCCESS)
        return Error(status);
    if (count == 0)
        return -ENODEV;
    std::unique_ptr<VkPhysicalDevice[]> devices(new (std::nothrow) VkPhysicalDevice[count]);
    if (!devices)
        return -ENOMEM;
    status = enumerateDevices(instance, &count, devices.get());
    if (status != VK_SUCCESS)
        return Error(status);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceIDProperties ids {};
        ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceDriverProperties driver {};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        driver.pNext = &ids;
        VkPhysicalDeviceProperties2 properties {};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &driver;
        getProperties(devices[i], &properties);
        if (properties.properties.vendorID != 0x5143 || driver.driverID != VK_DRIVER_ID_MESA_TURNIP ||
            properties.properties.apiVersion < VK_API_VERSION_1_2 ||
            (uuid != nullptr && std::memcmp(uuid, ids.deviceUUID, VK_UUID_SIZE) != 0))
            continue;
        if (physical != VK_NULL_HANDLE)
            return -ENOTUNIQ;
        physical = devices[i];
    }
    if (physical == VK_NULL_HANDLE)
        return -ENODEV;
    count = 0;
    status = enumerateExtensions(physical, nullptr, &count, nullptr);
    if (status != VK_SUCCESS)
        return Error(status);
    if (count == 0)
        return -ENOTSUP;
    std::unique_ptr<VkExtensionProperties[]> extensions(new (std::nothrow) VkExtensionProperties[count]);
    if (count != 0 && !extensions)
        return -ENOMEM;
    status = enumerateExtensions(physical, nullptr, &count, extensions.get());
    if (status != VK_SUCCESS)
        return Error(status);
    const char* needed[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
    for (const char* name : needed) {
        if (!Extension(extensions.get(), count, name))
            return -ENOTSUP;
    }
    count = 0;
    getQueues(physical, &count, nullptr);
    if (count == 0)
        return -ENOTSUP;
    std::unique_ptr<VkQueueFamilyProperties[]> queues(new (std::nothrow) VkQueueFamilyProperties[count]);
    if (!queues)
        return -ENOMEM;
    getQueues(physical, &count, queues.get());
    uint32_t family = 0;
    while (family < count && (!(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || queues[family].queueCount == 0))
        family++;
    if (family == count)
        return -ENOTSUP;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures conversion {};
    conversion.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceFeatures2 features {};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = &conversion;
    getFeatures(physical, &features);
    ycbcr = conversion.samplerYcbcrConversion == VK_TRUE;
    float priority = 0.0f;
    VkDeviceQueueCreateInfo queue {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &conversion;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = 2;
    deviceInfo.ppEnabledExtensionNames = needed;
    VkDevice createdDevice = VK_NULL_HANDLE;
    status = createDevice(physical, &deviceInfo, nullptr, &createdDevice);
    if (status != VK_SUCCESS)
        return Error(status);
    device = createdDevice;
#define DEVICE_PROC(type, name) name = reinterpret_cast<PFN_##type>(getDeviceProcAddr(device, #type))
    DEVICE_PROC(vkCreateImage, createImage);
    DEVICE_PROC(vkDestroyImage, destroyImage);
    DEVICE_PROC(vkGetImageMemoryRequirements, getRequirements);
    DEVICE_PROC(vkGetImageSubresourceLayout, getLayout);
    DEVICE_PROC(vkAllocateMemory, allocateMemory);
    DEVICE_PROC(vkFreeMemory, freeMemory);
    DEVICE_PROC(vkBindImageMemory, bindMemory);
    DEVICE_PROC(vkGetMemoryFdKHR, getMemoryFd);
#undef DEVICE_PROC
    if (!createImage || !destroyImage || !getRequirements || !getLayout || !allocateMemory || !freeMemory ||
        !bindMemory || !getMemoryFd)
        return -EPROTO;
    getMemoryProperties(physical, &memoryProperties);
    if (memoryProperties.memoryTypeCount == 0 || memoryProperties.memoryTypeCount > VK_MAX_MEMORY_TYPES)
        return -EPROTO;
    return 0;
}

int VulkanBufferAllocator::Impl::Prepare(const NativeBufferRequest& request, Image& image, Format& format,
    VkMemoryRequirements& requirements, uint32_t& memoryType) const noexcept
{
    constexpr uint64_t SUPPORTED_USAGE = HBM_USE_CPU_READ | HBM_USE_CPU_WRITE | HBM_USE_MEM_DMA |
        HBM_USE_MEM_MMZ_CACHE | HBM_USE_HW_RENDER | HBM_USE_HW_TEXTURE | HBM_USE_HW_COMPOSER;
    format = NativeFormat(request.format);
    if (request.width == 0 || request.height == 0 || request.width > INT32_MAX || request.height > INT32_MAX)
        return -EINVAL;
    if (format.planes == 0 || (request.usage & ~SUPPORTED_USAGE) ||
        (format.planes > 1 && (!ycbcr || (request.width & 1) || (request.height & 1))))
        return -ENOTSUP;
    VkImageUsageFlags usage = 0;
    if (request.usage & HBM_USE_HW_RENDER)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (request.usage & HBM_USE_HW_TEXTURE)
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (request.usage & HBM_USE_CPU_READ)
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (request.usage & HBM_USE_CPU_WRITE)
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage == 0)
        usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkPhysicalDeviceExternalImageFormatInfo external {};
    external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 query {};
    query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    query.pNext = &external;
    query.format = format.vk;
    query.type = VK_IMAGE_TYPE_2D;
    query.tiling = VK_IMAGE_TILING_LINEAR;
    query.usage = usage;
    VkExternalImageFormatProperties externalProperties {};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    properties.pNext = &externalProperties;
    VkResult status = getImageProperties(physical, &query, &properties);
    if (status != VK_SUCCESS)
        return Error(status);
    const auto& caps = externalProperties.externalMemoryProperties;
    constexpr auto REQUIRED = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
    if ((caps.externalMemoryFeatures & REQUIRED) != REQUIRED ||
        !(caps.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) ||
        request.width > properties.imageFormatProperties.maxExtent.width ||
        request.height > properties.imageFormatProperties.maxExtent.height ||
        !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
        return -ENOTSUP;
    VkExternalMemoryImageCreateInfo externalImage {};
    externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    externalImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.pNext = &externalImage;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format.vk;
    info.extent = {request.width, request.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_LINEAR;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage createdImage = VK_NULL_HANDLE;
    status = createImage(device, &info, nullptr, &createdImage);
    if (status != VK_SUCCESS)
        return Error(status);
    image.image = createdImage;
    getRequirements(device, image.image, &requirements);
    if (requirements.size == 0 || requirements.size > INT32_MAX)
        return -EOVERFLOW;
    VkMemoryPropertyFlags wanted = 0;
    if (request.usage & (HBM_USE_CPU_READ | HBM_USE_CPU_WRITE | HBM_USE_MEM_MMZ_CACHE))
        wanted |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (request.usage & HBM_USE_MEM_MMZ_CACHE)
        wanted |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            memoryType = i;
            return 0;
        }
    }
    return -ENOTSUP;
}

VulkanBufferAllocator::~VulkanBufferAllocator()
{
    delete impl_;
}

int VulkanBufferAllocator::Initialize(const char* loaderLibrary, const uint8_t* deviceUuid) noexcept
{
    if (loaderLibrary == nullptr || *loaderLibrary == '\0')
        return -EINVAL;
    if (impl_ != nullptr)
        return -EBUSY;
    std::unique_ptr<Impl> implementation(new (std::nothrow) Impl);
    if (!implementation)
        return -ENOMEM;
    int error = implementation->Open(loaderLibrary, deviceUuid);
    if (error == 0)
        impl_ = implementation.release();
    return error;
}

int VulkanBufferAllocator::IsSupported(const NativeBufferRequest& request, bool& supported) const noexcept
{
    supported = false;
    if (impl_ == nullptr)
        return -ENODEV;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Image image {*impl_};
    Format format;
    VkMemoryRequirements requirements;
    uint32_t memoryType;
    int error = impl_->Prepare(request, image, format, requirements, memoryType);
    if (error == -ENOTSUP || error == -EINVAL)
        return 0;
    if (error == 0)
        supported = true;
    return error;
}

int VulkanBufferAllocator::Allocate(const NativeBufferRequest& request, BufferHandle*& result) const noexcept
{
    if (result != nullptr)
        return -EBUSY;
    if (impl_ == nullptr)
        return -ENODEV;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    Impl::Image image {*impl_};
    Format format;
    VkMemoryRequirements requirements;
    uint32_t memoryType;
    int error = impl_->Prepare(request, image, format, requirements, memoryType);
    if (error != 0)
        return error;
    VkMemoryDedicatedAllocateInfo dedicated {};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image.image;
    VkExportMemoryAllocateInfo exportInfo {};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.pNext = &dedicated;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo allocation {};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.pNext = &exportInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    VkDeviceMemory allocated = VK_NULL_HANDLE;
    VkResult status = impl_->allocateMemory(impl_->device, &allocation, nullptr, &allocated);
    if (status != VK_SUCCESS)
        return Error(status);
    image.memory = allocated;
    status = impl_->bindMemory(impl_->device, image.image, image.memory, 0);
    if (status != VK_SUCCESS)
        return Error(status);
    VkMemoryGetFdInfoKHR exportFd {};
    exportFd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    exportFd.memory = image.memory;
    exportFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    status = impl_->getMemoryFd(impl_->device, &exportFd, &fd);
    if (status != VK_SUCCESS)
        return Error(status);
    if (fd < 0)
        return -EPROTO;
    DmaBuffer buffer;
    int fdFlags = fcntl(fd, F_GETFD);
    error = fdFlags < 0 ? -errno : ((fdFlags & FD_CLOEXEC) ? DmaBuffer::Duplicate(fd, buffer) : -EPROTO);
    close(fd);
    if (error != 0)
        return error;
    if (buffer.Size() < requirements.size)
        return -EPROTO;
    QcomBufferLayout layout {};
    layout.drm_format = qcom_native_format_to_drm(request.format);
    layout.modifier = DRM_FORMAT_MOD_LINEAR;
    layout.plane_count = format.planes;
    layout.allocation_size = buffer.Size();
    VkSubresourceLayout planes[QCOM_BUFFER_MAX_PLANES] {};
    for (uint32_t i = 0; i < format.planes; i++) {
        VkImageSubresource subresource {};
        subresource.aspectMask = format.planes == 1 ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT) :
            static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_PLANE_0_BIT << i);
        impl_->getLayout(impl_->device, image.image, &subresource, &planes[i]);
    }
    for (uint32_t i = 0; i < format.planes; i++) {
        uint64_t end = i + 1 < format.planes ? planes[i + 1].offset : buffer.Size();
        if (end > buffer.Size() || planes[i].offset >= end || planes[i].size == 0 ||
            planes[i].size > end - planes[i].offset)
            return -EPROTO;
        // Include the driver's inter-plane/tail padding, not just the texel span.
        layout.planes[i] = {planes[i].offset, planes[i].rowPitch, end - planes[i].offset};
    }
    return ExportNativeBuffer(buffer, request.width, request.height, request.format, request.usage, layout, result);
}

} // namespace OHOS::Qcom
