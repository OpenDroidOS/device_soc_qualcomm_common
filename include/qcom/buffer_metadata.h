/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QCOM_BUFFER_METADATA_H
#define QCOM_BUFFER_METADATA_H

#include "qcom/native_buffer.h"
#include <vector>

namespace OHOS::Qcom {

class BufferMetadata final {
public:
    // Replace a new, unmapped owned layout-only handle with a metadata-backed handle.
    static int Attach(BufferHandle*& handle) noexcept;
    // Retain both descriptors and the validated immutable handle fields.
    static int Clone(const BufferHandle& handle, BufferHandle*& result) noexcept;
    static int Validate(const BufferHandle& handle) noexcept;
    static int Set(const BufferHandle& handle, uint32_t key, const std::vector<uint8_t>& value) noexcept;
    static int Get(const BufferHandle& handle, uint32_t key, std::vector<uint8_t>& value) noexcept;
    static int List(const BufferHandle& handle, std::vector<uint32_t>& keys) noexcept;
    static int Erase(const BufferHandle& handle, uint32_t key) noexcept;
    static int BeginCpuAccess(const BufferHandle& handle, uint64_t* session = nullptr) noexcept;
    static int EndCpuAccess(const BufferHandle& handle, bool ownerOnly = false,
                            uint64_t expectedSession = 0) noexcept;
};

} // namespace OHOS::Qcom
#endif
