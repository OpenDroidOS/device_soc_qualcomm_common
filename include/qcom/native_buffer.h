/*
 * Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef QCOM_NATIVE_BUFFER_H
#define QCOM_NATIVE_BUFFER_H

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "buffer_handle.h"
#include <drm_fourcc.h>
#include <native_buffer/buffer_common.h>

#if UINTPTR_MAX == UINT64_MAX
#ifdef __cplusplus
static_assert(offsetof(BufferHandle, reserve) == 56, "Use the native HDI BufferHandle header");
#else
_Static_assert(offsetof(BufferHandle, reserve) == 56, "Use the native HDI BufferHandle header");
#endif
#endif

/* Reserved-int transport for buffers exported by the shared native allocator.
 * Each 64-bit value is stored low word first, without C struct padding.
 * Other providers' handles must be queried through their own layout contract.
 */
#define QCOM_BUFFER_LAYOUT_MAGIC UINT32_C(0x51434d42)
#define QCOM_BUFFER_LAYOUT_VERSION 1u
#define QCOM_BUFFER_METADATA_VERSION 2u
#define QCOM_BUFFER_MAX_PLANES 4u
#define QCOM_BUFFER_LAYOUT_WORDS (8u + 6u * QCOM_BUFFER_MAX_PLANES)
#define QCOM_BUFFER_METADATA_WORDS (QCOM_BUFFER_LAYOUT_WORDS + 6u)

static inline const int32_t *qcom_buffer_layout_words(const BufferHandle *handle)
{
    if (!handle || !((handle->reserveFds == 0 && handle->reserveInts == QCOM_BUFFER_LAYOUT_WORDS) ||
        (handle->reserveFds == 1 && handle->reserveInts == QCOM_BUFFER_METADATA_WORDS && handle->reserve[0] >= 0)))
        return NULL;
    return handle->reserve + handle->reserveFds;
}

struct QcomBufferPlane {
    uint64_t offset;
    uint64_t row_pitch;
    uint64_t size;
};

struct QcomBufferLayout {
    uint32_t drm_format;
    uint32_t plane_count;
    uint64_t modifier;
    uint64_t allocation_size;
    struct QcomBufferPlane planes[QCOM_BUFFER_MAX_PLANES];
};

static inline uint32_t qcom_native_format_to_drm(int32_t format)
{
    switch (format) {
        case NATIVEBUFFER_PIXEL_FMT_RGBA_8888: return DRM_FORMAT_ABGR8888;
        case NATIVEBUFFER_PIXEL_FMT_RGBX_8888: return DRM_FORMAT_XBGR8888;
        case NATIVEBUFFER_PIXEL_FMT_BGRA_8888: return DRM_FORMAT_ARGB8888;
        case NATIVEBUFFER_PIXEL_FMT_BGRX_8888: return DRM_FORMAT_XRGB8888;
        case NATIVEBUFFER_PIXEL_FMT_RGB_565: return DRM_FORMAT_RGB565;
        case NATIVEBUFFER_PIXEL_FMT_BGR_565: return DRM_FORMAT_BGR565;
        case NATIVEBUFFER_PIXEL_FMT_RGB_888: return DRM_FORMAT_BGR888;
        case NATIVEBUFFER_PIXEL_FMT_RGBA_1010102: return DRM_FORMAT_ABGR2101010;
        case NATIVEBUFFER_PIXEL_FMT_RGBA16_FLOAT: return DRM_FORMAT_ABGR16161616F;
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP: return DRM_FORMAT_NV12;
        case NATIVEBUFFER_PIXEL_FMT_YCRCB_420_SP: return DRM_FORMAT_NV21;
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_420_P: return DRM_FORMAT_YUV420;
        case NATIVEBUFFER_PIXEL_FMT_YCRCB_420_P: return DRM_FORMAT_YVU420;
        case NATIVEBUFFER_PIXEL_FMT_YCBCR_P010: return DRM_FORMAT_P010;
        default: return 0;
    }
}

static inline uint64_t qcom_buffer_read_u64(const int32_t *words)
{
    return (uint64_t)(uint32_t)words[0] | ((uint64_t)(uint32_t)words[1] << 32);
}

static inline void qcom_buffer_write_u64(int32_t *words, uint64_t value)
{
    uint32_t low = (uint32_t)value, high = (uint32_t)(value >> 32);
    memcpy(words, &low, sizeof(low));
    memcpy(words + 1, &high, sizeof(high));
}

static inline int qcom_buffer_validate_linear_geometry(const BufferHandle *handle,
                                                        const struct QcomBufferLayout *layout)
{
    uint32_t count = 1, bytes = 0;
    switch (layout->drm_format) {
        case DRM_FORMAT_ABGR8888: case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XBGR8888: case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ABGR2101010: bytes = 4; break;
        case DRM_FORMAT_RGB565: case DRM_FORMAT_BGR565: bytes = 2; break;
        case DRM_FORMAT_BGR888: bytes = 3; break;
        case DRM_FORMAT_ABGR16161616F: bytes = 8; break;
        case DRM_FORMAT_NV12: case DRM_FORMAT_NV21: bytes = 1; count = 2; break;
        case DRM_FORMAT_YUV420: case DRM_FORMAT_YVU420: bytes = 1; count = 3; break;
        case DRM_FORMAT_P010: bytes = 2; count = 2; break;
        default: return -EINVAL;
    }
    if (layout->plane_count != count)
        return -EINVAL;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t columns = (uint32_t)handle->width, rows = (uint32_t)handle->height;
        uint32_t pixel_bytes = bytes;
        if (i != 0) {
            columns = (columns + 1) / 2;
            rows = (rows + 1) / 2;
            if (count == 2)
                pixel_bytes *= 2;
        }
        uint64_t row_bytes = columns * pixel_bytes;
        const struct QcomBufferPlane *plane = &layout->planes[i];
        // Width/height and pitch are bounded signed 32-bit values; products fit u64.
        if (row_bytes > plane->row_pitch || (rows - 1) * plane->row_pitch + row_bytes > plane->size)
            return -EINVAL;
    }
    return 0;
}

static inline int qcom_buffer_validate_layout(const BufferHandle *handle,
                                              const struct QcomBufferLayout *layout)
{
    if (!handle || !layout || handle->fd < 0 || handle->width <= 0 || handle->height <= 0 ||
        handle->stride <= 0 || handle->size <= 0 || qcom_buffer_layout_words(handle) == NULL ||
        layout->plane_count == 0 ||
        layout->plane_count > QCOM_BUFFER_MAX_PLANES ||
        layout->allocation_size != (uint64_t)handle->size ||
        layout->drm_format == 0 || layout->drm_format != qcom_native_format_to_drm(handle->format) ||
        layout->modifier == DRM_FORMAT_MOD_INVALID ||
        layout->planes[0].row_pitch != (uint64_t)handle->stride)
        return -EINVAL;

    for (uint32_t i = 0; i < layout->plane_count; i++) {
        const struct QcomBufferPlane *plane = &layout->planes[i];
        if (plane->row_pitch == 0 || plane->row_pitch > INT32_MAX || plane->size == 0 ||
            plane->offset >= layout->allocation_size ||
            plane->size > layout->allocation_size - plane->offset)
            return -EINVAL;
        for (uint32_t j = 0; j < i; j++) {
            const struct QcomBufferPlane *other = &layout->planes[j];
            if (plane->offset < other->offset + other->size &&
                other->offset < plane->offset + plane->size)
                return -EINVAL;
        }
    }
    return layout->modifier == DRM_FORMAT_MOD_LINEAR ? qcom_buffer_validate_linear_geometry(handle, layout) : 0;
}

static inline int qcom_buffer_read_layout(const BufferHandle *handle, struct QcomBufferLayout *out)
{
    const int32_t *words = qcom_buffer_layout_words(handle);
    if (!words || !out)
        return -EINVAL;
    if ((uint32_t)words[0] != QCOM_BUFFER_LAYOUT_MAGIC ||
        (uint32_t)words[1] != (handle->reserveFds == 0 ? QCOM_BUFFER_LAYOUT_VERSION : QCOM_BUFFER_METADATA_VERSION) ||
        (handle->reserveFds == 1 && qcom_buffer_read_u64(words + QCOM_BUFFER_LAYOUT_WORDS + 4) == 0))
        return -EINVAL;
    struct QcomBufferLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.drm_format = (uint32_t)words[2];
    layout.plane_count = (uint32_t)words[3];
    layout.modifier = qcom_buffer_read_u64(words + 4);
    layout.allocation_size = qcom_buffer_read_u64(words + 6);
    if (layout.plane_count > QCOM_BUFFER_MAX_PLANES)
        return -EINVAL;
    for (uint32_t i = 0; i < layout.plane_count; i++) {
        layout.planes[i].offset = qcom_buffer_read_u64(words + 8 + 6 * i);
        layout.planes[i].row_pitch = qcom_buffer_read_u64(words + 10 + 6 * i);
        layout.planes[i].size = qcom_buffer_read_u64(words + 12 + 6 * i);
    }
    int result = qcom_buffer_validate_layout(handle, &layout);
    if (result == 0)
        *out = layout;
    return result;
}

static inline int qcom_buffer_write_layout(BufferHandle *handle, const struct QcomBufferLayout *layout)
{
    int result = qcom_buffer_validate_layout(handle, layout);
    if (result != 0)
        return result;
    int32_t *words = handle->reserve + handle->reserveFds;
    memset(words, 0, QCOM_BUFFER_LAYOUT_WORDS * sizeof(*words));
    words[0] = (int32_t)QCOM_BUFFER_LAYOUT_MAGIC;
    words[1] = handle->reserveFds == 0 ? QCOM_BUFFER_LAYOUT_VERSION : QCOM_BUFFER_METADATA_VERSION;
    memcpy(words + 2, &layout->drm_format, sizeof(layout->drm_format));
    words[3] = (int32_t)layout->plane_count;
    qcom_buffer_write_u64(words + 4, layout->modifier);
    qcom_buffer_write_u64(words + 6, layout->allocation_size);
    for (uint32_t i = 0; i < layout->plane_count; i++) {
        qcom_buffer_write_u64(words + 8 + 6 * i, layout->planes[i].offset);
        qcom_buffer_write_u64(words + 10 + 6 * i, layout->planes[i].row_pitch);
        qcom_buffer_write_u64(words + 12 + 6 * i, layout->planes[i].size);
    }
    return 0;
}

#endif
