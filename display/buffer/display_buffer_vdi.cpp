/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcom/buffer_metadata.h"
#include "qcom/native_buffer_export.h"
#include "qcom/vulkan_buffer_allocator.h"
#include "idisplay_buffer_vdi.h"
#include "display_type.h"
#include "hdf_base.h"

#include <cstdlib>
#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <sys/mman.h>
#include <sys/stat.h>
#include <tuple>
#include <unordered_map>
#include <unistd.h>

#ifndef QCOM_VULKAN_LOADER_LIBRARY
#define QCOM_VULKAN_LOADER_LIBRARY "libvulkan.so"
#endif

namespace OHOS::Qcom {
namespace {
namespace Hdi = OHOS::HDI::Display::Buffer::V1_0;
namespace HdiLayout = OHOS::HDI::Display::Buffer::V1_2;

int32_t Result(int error) noexcept
{
    switch (error) {
        case 0: return HDF_SUCCESS;
        case -EINVAL: case -EBADMSG: return HDF_ERR_INVALID_PARAM;
        case -ENOTSUP: case -ENODATA: return HDF_ERR_NOT_SUPPORT;
        case -ENOMEM: return HDF_ERR_MALLOC_FAIL;
        case -EAGAIN: case -EBUSY: return HDF_ERR_DEVICE_BUSY;
        case -EBADF: return HDF_ERR_BAD_FD;
        case -EACCES: case -EPERM: return HDF_ERR_NOPERM;
        default: return HDF_ERR_IO;
    }
}

struct Mapping {
    DmaBuffer pixels;
    int metadataFd = -1;
    std::array<uint64_t, 4> identity {};
    uint64_t cpuSession = 0;
    uint32_t cpuPid = 0;
    void* address = MAP_FAILED;
    size_t length = 0;
    ~Mapping() {
        if (address != MAP_FAILED) munmap(address, length);
        if (metadataFd >= 0) close(metadataFd);
    }
};
using LeaseKey = std::tuple<std::array<uint64_t, 4>, uint64_t, uint32_t>;
struct Mappings {
    std::mutex mutex;
    std::unordered_map<const BufferHandle*, std::unique_ptr<Mapping>> buffers;
    std::map<LeaseKey, uint32_t> cpuLeases;
};
Mappings& Registry()
{
    static Mappings mappings;
    return mappings;
}

int Identity(int pixels, int metadata, std::array<uint64_t, 4>& identity) noexcept
{
    struct stat first {}, second {};
    if (fstat(pixels, &first) < 0 || fstat(metadata, &second) < 0) return -errno;
    identity = {static_cast<uint64_t>(first.st_dev), static_cast<uint64_t>(first.st_ino),
                static_cast<uint64_t>(second.st_dev), static_cast<uint64_t>(second.st_ino)};
    return 0;
}

int GetMappingLocked(const BufferHandle& handle, Mappings& registry, Mapping*& output) noexcept
{
    if (handle.reserveFds != 1) return -EINVAL;
    std::array<uint64_t, 4> identity;
    int error = Identity(handle.fd, handle.reserve[0], identity);
    if (error != 0) return error;
    auto found = registry.buffers.find(&handle);
    if (found != registry.buffers.end()) {
        if (found->second->identity != identity) return -EINVAL;
        output = found->second.get();
        return 0;
    }
    std::unique_ptr<Mapping> mapping(new (std::nothrow) Mapping);
    if (!mapping) return -ENOMEM;
    error = DmaBuffer::Duplicate(handle.fd, mapping->pixels);
    if (error != 0) return error;
    do { mapping->metadataFd = fcntl(handle.reserve[0], F_DUPFD_CLOEXEC, 0); }
    while (mapping->metadataFd < 0 && errno == EINTR);
    if (mapping->metadataFd < 0) return -errno;
    error = Identity(mapping->pixels.Fd(), mapping->metadataFd, mapping->identity);
    if (error != 0) return error;
    if (mapping->identity != identity || mapping->pixels.Size() != static_cast<uint64_t>(handle.size)) return -EINVAL;
    mapping->length = static_cast<size_t>(handle.size);
    output = mapping.get();
    registry.buffers.emplace(&handle, std::move(mapping));
    return 0;
}

LeaseKey Key(const Mapping& mapping)
{
    return {mapping.identity, mapping.cpuSession, mapping.cpuPid};
}

void DropLeaseLocked(Mappings& registry, Mapping& mapping)
{
    if (mapping.cpuSession == 0) return;
    auto lease = registry.cpuLeases.find(Key(mapping));
    if (lease != registry.cpuLeases.end() && --lease->second == 0) registry.cpuLeases.erase(lease);
    mapping.cpuSession = 0;
    mapping.cpuPid = 0;
}

int FinishLeaseLocked(const BufferHandle& handle, Mappings& registry, Mapping& mapping) noexcept
{
    if (mapping.cpuSession == 0) return 0;
    auto lease = registry.cpuLeases.find(Key(mapping));
    if (lease == registry.cpuLeases.end()) return -EINVAL;
    if (lease->second == 1 && mapping.cpuPid == static_cast<uint32_t>(getpid())) {
        int error = BufferMetadata::EndCpuAccess(handle, true, mapping.cpuSession);
        if (error != 0) return error;
    }
    DropLeaseLocked(registry, mapping);
    return 0;
}

uint32_t PixelStride(uint32_t format, uint32_t plane) noexcept
{
    switch (format) {
        case DRM_FORMAT_ABGR8888: case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XBGR8888: case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ABGR2101010: return 4;
        case DRM_FORMAT_RGB565: case DRM_FORMAT_BGR565: return 2;
        case DRM_FORMAT_BGR888: return 3;
        case DRM_FORMAT_ABGR16161616F: return 8;
        case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: return plane == 0 ? 1 : 2;
        case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: return 1;
        case DRM_FORMAT_P010: return plane == 0 ? 2 : 4;
        default: return 0;
    }
}

int Clone(const BufferHandle& input, BufferHandle*& output) noexcept
{
    return BufferMetadata::Clone(input, output);
}

class DisplayBufferVdi final : public Hdi::IDisplayBufferVdi {
public:
    int32_t AllocMem(const Hdi::AllocInfo& info, BufferHandle*& handle) const override
    {
        if (handle != nullptr) return HDF_ERR_INVALID_PARAM;
        if (info.expectedSize != 0 || info.format > INT32_MAX) return HDF_ERR_NOT_SUPPORT;
        std::lock_guard<std::mutex> lock(allocatorMutex_);
        int error = Initialize();
        if (error != 0) return Result(error);
        BufferHandle* result = nullptr;
        error = allocator_->Allocate({info.width, info.height, static_cast<int32_t>(info.format), info.usage}, result);
        if (error != 0) return Result(error);
        error = BufferMetadata::Attach(result);
        if (error != 0) { FreeNativeBuffer(result); return Result(error); }
        handle = result;
        return HDF_SUCCESS;
    }

    void FreeMem(const BufferHandle& handle) const override
    {
        int32_t result = Unmap(handle);
        if (result != HDF_SUCCESS) {
            std::fprintf(stderr, "Native display buffer cleanup failed: %d\n", result);
            auto& registry = Registry();
            std::lock_guard<std::mutex> lock(registry.mutex);
            auto found = registry.buffers.find(&handle);
            if (found != registry.buffers.end()) {
                DropLeaseLocked(registry, *found->second);
                registry.buffers.erase(found);
            }
            const_cast<BufferHandle&>(handle).virAddr = nullptr;
        }
        FreeNativeBuffer(const_cast<BufferHandle*>(&handle));
    }

    void* Mmap(const BufferHandle& handle) const override
    {
        QcomBufferLayout layout;
        int error = BufferMetadata::Validate(handle);
        if (error != 0 || qcom_buffer_read_layout(&handle, &layout) != 0 ||
            layout.modifier != DRM_FORMAT_MOD_LINEAR || (handle.usage & HBM_USE_PROTECTED)) {
            errno = error != 0 ? -error : EINVAL;
            return nullptr;
        }
        auto& registry = Registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        Mapping* mapping = nullptr;
        error = GetMappingLocked(handle, registry, mapping);
        if (error != 0) { errno = -error; return nullptr; }
        if (mapping->address == MAP_FAILED) {
            mapping->address = mmap(nullptr, mapping->length, PROT_READ | PROT_WRITE, MAP_SHARED, mapping->pixels.Fd(), 0);
            if (mapping->address == MAP_FAILED) return nullptr;
        }
        const_cast<BufferHandle&>(handle).virAddr = mapping->address;
        return mapping->address;
    }

    int32_t Unmap(const BufferHandle& handle) const override
    {
        auto& registry = Registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto found = registry.buffers.find(&handle);
        if (found == registry.buffers.end()) return handle.virAddr == nullptr ? HDF_SUCCESS : HDF_ERR_INVALID_PARAM;
        std::array<uint64_t, 4> identity;
        if (handle.reserveFds != 1) return HDF_ERR_INVALID_PARAM;
        int error = Identity(handle.fd, handle.reserve[0], identity);
        if (error != 0) return Result(error);
        if (found->second->identity != identity) return HDF_ERR_INVALID_PARAM;
        error = FinishLeaseLocked(handle, registry, *found->second);
        if (error != 0) return Result(error);
        if (found->second->address != MAP_FAILED && munmap(found->second->address, found->second->length) < 0)
            return Result(-errno);
        found->second->address = MAP_FAILED;
        registry.buffers.erase(found);
        const_cast<BufferHandle&>(handle).virAddr = nullptr;
        return HDF_SUCCESS;
    }

    int32_t FlushCache(const BufferHandle& handle) const override { return Result(BufferMetadata::EndCpuAccess(handle)); }
    int32_t InvalidateCache(const BufferHandle& handle) const override
    {
        int error = BufferMetadata::Validate(handle);
        if (error != 0) return Result(error);
        auto& registry = Registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        Mapping* mapping = nullptr;
        error = GetMappingLocked(handle, registry, mapping);
        if (error != 0) return Result(error);
        uint64_t session = 0;
        error = BufferMetadata::BeginCpuAccess(handle, &session);
        if (error != 0) return Result(error);
        uint32_t pid = static_cast<uint32_t>(getpid());
        if (mapping->cpuSession != session || mapping->cpuPid != pid) {
            DropLeaseLocked(registry, *mapping);
            mapping->cpuSession = session;
            mapping->cpuPid = pid;
            ++registry.cpuLeases[Key(*mapping)];
        }
        return HDF_SUCCESS;
    }
    int32_t RegisterBuffer(const BufferHandle& handle) override { return Result(BufferMetadata::Validate(handle)); }
    int32_t SetMetadata(const BufferHandle& handle, uint32_t key, const std::vector<uint8_t>& value) override
    { return Result(BufferMetadata::Set(handle, key, value)); }
    int32_t GetMetadata(const BufferHandle& handle, uint32_t key, std::vector<uint8_t>& value) override
    { return Result(BufferMetadata::Get(handle, key, value)); }
    int32_t ListMetadataKeys(const BufferHandle& handle, std::vector<uint32_t>& keys) override
    { return Result(BufferMetadata::List(handle, keys)); }
    int32_t EraseMetadataKey(const BufferHandle& handle, uint32_t key) override
    { return Result(BufferMetadata::Erase(handle, key)); }

    int32_t GetImageLayout(const BufferHandle& handle, HdiLayout::ImageLayout& output) const override
    {
        QcomBufferLayout layout;
        int error = BufferMetadata::Validate(handle);
        if (error != 0) return Result(error);
        if (qcom_buffer_read_layout(&handle, &layout) != 0) return HDF_ERR_INVALID_PARAM;
        if (layout.modifier != DRM_FORMAT_MOD_LINEAR) return HDF_ERR_NOT_SUPPORT;
        HdiLayout::ImageLayout result;
        result.pixelFormat = static_cast<uint32_t>(handle.format);
        for (uint32_t i = 0; i < layout.plane_count; i++) {
            uint32_t pixelStride = PixelStride(layout.drm_format, i);
            if (layout.planes[i].offset > UINT32_MAX || layout.planes[i].row_pitch > UINT32_MAX || pixelStride == 0)
                return HDF_ERR_NOT_SUPPORT;
            result.planes.push_back({static_cast<uint32_t>(layout.planes[i].offset),
                static_cast<uint32_t>(layout.planes[i].row_pitch), pixelStride});
        }
        output = std::move(result);
        return HDF_SUCCESS;
    }

    int32_t IsSupportedAlloc(const std::vector<Hdi::VerifyAllocInfo>& infos, std::vector<bool>& output) const override
    {
        std::lock_guard<std::mutex> lock(allocatorMutex_);
        int error = Initialize();
        if (error != 0) return Result(error);
        std::vector<bool> supported;
        for (const auto& info : infos) {
            bool value = false;
            if (info.format <= INT32_MAX) {
                error = allocator_->IsSupported({info.width, info.height, static_cast<int32_t>(info.format), info.usage}, value);
                if (error != 0) return Result(error);
            }
            supported.push_back(value);
        }
        output.swap(supported);
        return HDF_SUCCESS;
    }

    int32_t IsSupportAllocPassthrough(const Hdi::AllocInfo& info) const override
    {
        if (info.expectedSize != 0) return HDF_ERR_NOT_SUPPORT;
        std::vector<Hdi::VerifyAllocInfo> requests {{info.width, info.height, info.usage, info.format}};
        std::vector<bool> supported;
        int32_t result = IsSupportedAlloc(requests, supported);
        return result != HDF_SUCCESS ? result : supported[0] ? HDF_SUCCESS : HDF_ERR_NOT_SUPPORT;
    }

    int32_t ReAllocMem(const Hdi::AllocInfo& info, const BufferHandle&, BufferHandle*& output) const override
    { return AllocMem(info, output); }
    int32_t CloneDmaBufferHandle(const BufferHandle& input, BufferHandle*& output) const override
    { return Result(Clone(input, output)); }

private:
    int Initialize() const noexcept
    {
        if (allocator_) return 0;
        std::unique_ptr<VulkanBufferAllocator> allocator(new (std::nothrow) VulkanBufferAllocator);
        if (!allocator) return -ENOMEM;
        int error = allocator->Initialize(QCOM_VULKAN_LOADER_LIBRARY);
        if (error == 0) allocator_ = std::move(allocator);
        return error;
    }
    mutable std::mutex allocatorMutex_;
    mutable std::unique_ptr<VulkanBufferAllocator> allocator_;
};
} // namespace
} // namespace OHOS::Qcom

namespace OHOS::HDI::Display::Buffer::V1_0 {
extern "C" __attribute__((visibility("default"))) IDisplayBufferVdi* CreateDisplayBufferVdi()
{ return new (std::nothrow) OHOS::Qcom::DisplayBufferVdi(); }
extern "C" __attribute__((visibility("default"))) void DestroyDisplayBufferVdi(IDisplayBufferVdi* provider) { delete provider; }
} // namespace OHOS::HDI::Display::Buffer::V1_0
