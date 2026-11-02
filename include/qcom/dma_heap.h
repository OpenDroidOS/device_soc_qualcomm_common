/*
 * Copyright 2026 OpenDroidOS
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef QCOM_DMA_HEAP_H
#define QCOM_DMA_HEAP_H

#include <cstddef>
#include <cstdint>

namespace OHOS::Qcom {

enum class CpuAccess : uint64_t {
    READ = 1,
    WRITE = 2,
    READ_WRITE = 3,
};

class DmaBuffer;

class CpuMapping final {
public:
    CpuMapping() = default;
    ~CpuMapping();
    CpuMapping(const CpuMapping&) = delete;
    CpuMapping& operator=(const CpuMapping&) = delete;
    CpuMapping(CpuMapping&& other) noexcept;
    CpuMapping& operator=(CpuMapping&&) = delete;

    // Begin/End bracket CPU access only; callers separately wait device fences.
    int Begin() noexcept;
    int End() noexcept;
    int Finish() noexcept;
    void* Data() const noexcept { return active_ ? address_ : nullptr; }
    size_t Size() const noexcept { return length_; }
    bool IsActive() const noexcept { return active_; }

private:
    friend class DmaBuffer;
    void Cleanup() noexcept;
    int fd_ = -1;
    void* address_ = nullptr;
    size_t length_ = 0;
    uint64_t access_ = 0;
    bool active_ = false;
};

class DmaBuffer final {
public:
    DmaBuffer() = default;
    ~DmaBuffer();
    DmaBuffer(const DmaBuffer&) = delete;
    DmaBuffer& operator=(const DmaBuffer&) = delete;
    DmaBuffer(DmaBuffer&& other) noexcept;
    DmaBuffer& operator=(DmaBuffer&& other) noexcept;

    static int Duplicate(int fd, DmaBuffer& result) noexcept;
    int DuplicateFd() const noexcept;
    int Map(CpuAccess access, CpuMapping& result) const noexcept;
    int Fd() const noexcept { return fd_; } // Borrowed; use DuplicateFd for transfer.
    uint64_t Size() const noexcept { return size_; }

private:
    friend class DmaHeap;
    int fd_ = -1;
    uint64_t size_ = 0;
};

class DmaHeap final {
public:
    DmaHeap() = default;
    ~DmaHeap();
    DmaHeap(const DmaHeap&) = delete;
    DmaHeap& operator=(const DmaHeap&) = delete;
    DmaHeap(DmaHeap&& other) noexcept;
    DmaHeap& operator=(DmaHeap&& other) noexcept;

    static int Open(const char* node, DmaHeap& result) noexcept;
    int Allocate(uint64_t bytes, DmaBuffer& result) const noexcept;
    bool IsOpen() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

} // namespace OHOS::Qcom
#endif // QCOM_DMA_HEAP_H
