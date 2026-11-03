/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcom/native_buffer_export.h"

#include <cstdlib>
#include <unistd.h>

namespace OHOS::Qcom {

int ExportNativeBuffer(const DmaBuffer& buffer, uint32_t width, uint32_t height,
    int32_t format, uint64_t usage, const QcomBufferLayout& layout, BufferHandle*& result) noexcept
{
    if (result != nullptr)
        return -EBUSY;
    if (buffer.Fd() < 0 || width == 0 || height == 0 || width > INT32_MAX || height > INT32_MAX ||
        buffer.Size() > INT32_MAX || buffer.Size() != layout.allocation_size ||
        layout.planes[0].row_pitch > INT32_MAX)
        return -EINVAL;
    auto* handle = static_cast<BufferHandle*>(std::calloc(1,
        sizeof(BufferHandle) + QCOM_BUFFER_LAYOUT_WORDS * sizeof(int32_t)));
    if (handle == nullptr)
        return -ENOMEM;
    // Borrow only while validating; the returned handle always owns a duplicate.
    handle->fd = buffer.Fd();
    handle->width = static_cast<int32_t>(width);
    handle->height = static_cast<int32_t>(height);
    handle->stride = static_cast<int32_t>(layout.planes[0].row_pitch);
    handle->size = static_cast<int32_t>(buffer.Size());
    handle->format = format;
    handle->usage = usage;
    handle->reserveInts = QCOM_BUFFER_LAYOUT_WORDS;
    int error = qcom_buffer_write_layout(handle, &layout);
    if (error != 0) {
        std::free(handle);
        return error;
    }
    int fd = buffer.DuplicateFd();
    if (fd < 0) {
        std::free(handle);
        return fd;
    }
    handle->fd = fd;
    result = handle;
    return 0;
}

void FreeNativeBuffer(BufferHandle* handle) noexcept
{
    if (handle == nullptr)
        return;
    if (handle->fd >= 0)
        close(handle->fd);
    if (handle->reserveFds == 1 && handle->reserve[0] >= 0)
        close(handle->reserve[0]);
    std::free(handle);
}

} // namespace OHOS::Qcom
