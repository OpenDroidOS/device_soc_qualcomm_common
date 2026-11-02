/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QCOM_VULKAN_BUFFER_ALLOCATOR_H
#define QCOM_VULKAN_BUFFER_ALLOCATOR_H

#include "qcom/native_buffer.h"

namespace OHOS::Qcom {

struct NativeBufferRequest {
    uint32_t width;
    uint32_t height;
    int32_t format;
    uint64_t usage;
};

class VulkanBufferAllocator final {
public:
    VulkanBufferAllocator() = default;
    ~VulkanBufferAllocator();
    VulkanBufferAllocator(const VulkanBufferAllocator&) = delete;
    VulkanBufferAllocator& operator=(const VulkanBufferAllocator&) = delete;

    // deviceUuid is null for a unique Turnip device, or points to its 16-byte Vulkan UUID.
    int Initialize(const char* loaderLibrary, const uint8_t* deviceUuid = nullptr) noexcept;
    int IsSupported(const NativeBufferRequest& request, bool& supported) const noexcept;
    int Allocate(const NativeBufferRequest& request, BufferHandle*& result) const noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace OHOS::Qcom
#endif
