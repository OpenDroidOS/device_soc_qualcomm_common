/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QCOM_NATIVE_BUFFER_EXPORT_H
#define QCOM_NATIVE_BUFFER_EXPORT_H

#include "qcom/dma_heap.h"
#include "qcom/native_buffer.h"

namespace OHOS::Qcom {

// Export an owned handle with a duplicated fd and the allocator's actual layout.
// The caller supplies a layout derived for its selected backend, not an estimate.
int ExportNativeBuffer(const DmaBuffer& buffer, uint32_t width, uint32_t height,
    int32_t format, uint64_t usage, const QcomBufferLayout& layout, BufferHandle*& result) noexcept;
void FreeNativeBuffer(BufferHandle* handle) noexcept;

} // namespace OHOS::Qcom
#endif
