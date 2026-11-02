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

#include "qcom/dma_heap.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace OHOS::Qcom {
namespace {
int DuplicateCloexec(int fd) noexcept
{
    int result;
    do {
        result = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : result;
}

int Synchronize(int fd, uint64_t flags) noexcept
{
    dma_buf_sync sync {flags};
    int result;
    do {
        result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result < 0 ? -errno : 0;
}
} // namespace

CpuMapping::~CpuMapping()
{
    Cleanup();
}

CpuMapping::CpuMapping(CpuMapping&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), address_(std::exchange(other.address_, nullptr)),
      length_(std::exchange(other.length_, 0)), access_(std::exchange(other.access_, 0)),
      active_(std::exchange(other.active_, false))
{
}

int CpuMapping::Begin() noexcept
{
    if (fd_ < 0) {
        return -EINVAL;
    }
    if (active_) {
        return -EBUSY;
    }
    int result = Synchronize(fd_, access_ | DMA_BUF_SYNC_START);
    if (result == 0) {
        active_ = true;
    }
    return result;
}

int CpuMapping::End() noexcept
{
    if (!active_) {
        return -EINVAL;
    }
    int result = Synchronize(fd_, access_ | DMA_BUF_SYNC_END);
    if (result == 0) {
        active_ = false;
    }
    return result;
}

int CpuMapping::Finish() noexcept
{
    if (fd_ < 0) {
        return 0;
    }
    if (active_) {
        int result = End();
        if (result != 0) {
            return result;
        }
    }
    if (munmap(address_, length_) < 0) {
        return -errno;
    }
    // On Linux close must not be retried after EINTR: the fd may be reused.
    close(fd_);
    fd_ = -1;
    address_ = nullptr;
    length_ = 0;
    access_ = 0;
    return 0;
}

void CpuMapping::Cleanup() noexcept
{
    if (fd_ < 0) {
        return;
    }
    if (active_) {
        Synchronize(fd_, access_ | DMA_BUF_SYNC_END);
    }
    munmap(address_, length_);
    close(fd_);
}

DmaBuffer::~DmaBuffer()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}

DmaBuffer::DmaBuffer(DmaBuffer&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), size_(std::exchange(other.size_, 0))
{
}

DmaBuffer& DmaBuffer::operator=(DmaBuffer&& other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

int DmaBuffer::Duplicate(int fd, DmaBuffer& result) noexcept
{
    if (result.fd_ >= 0) {
        return -EBUSY;
    }
    int duplicate = DuplicateCloexec(fd);
    if (duplicate < 0) {
        return duplicate;
    }
    off_t size = lseek(duplicate, 0, SEEK_END);
    int error = size < 0 ? errno : EINVAL;
    if (size <= 0 || lseek(duplicate, 0, SEEK_SET) < 0) {
        if (size > 0) {
            error = errno;
        }
        close(duplicate);
        return -error;
    }
    result.fd_ = duplicate;
    result.size_ = static_cast<uint64_t>(size);
    return 0;
}

int DmaBuffer::DuplicateFd() const noexcept
{
    return DuplicateCloexec(fd_);
}

int DmaBuffer::Map(CpuAccess access, CpuMapping& result) const noexcept
{
    uint64_t flags = static_cast<uint64_t>(access);
    if (fd_ < 0 || flags == 0 || (flags & ~static_cast<uint64_t>(DMA_BUF_SYNC_RW)) != 0) {
        return -EINVAL;
    }
    if (result.fd_ >= 0) {
        return -EBUSY;
    }
    if (size_ > std::numeric_limits<size_t>::max()) {
        return -EOVERFLOW;
    }
    int duplicate = DuplicateFd();
    if (duplicate < 0) {
        return duplicate;
    }
    int protection = ((flags & DMA_BUF_SYNC_READ) ? PROT_READ : 0) |
        ((flags & DMA_BUF_SYNC_WRITE) ? PROT_WRITE : 0);
    void* address = mmap(nullptr, static_cast<size_t>(size_), protection, MAP_SHARED, duplicate, 0);
    if (address == MAP_FAILED) {
        int error = errno;
        close(duplicate);
        return -error;
    }
    int error = Synchronize(duplicate, flags | DMA_BUF_SYNC_START);
    if (error != 0) {
        munmap(address, static_cast<size_t>(size_));
        close(duplicate);
        return error;
    }
    result.fd_ = duplicate;
    result.address_ = address;
    result.length_ = static_cast<size_t>(size_);
    result.access_ = flags;
    result.active_ = true;
    return 0;
}

DmaHeap::~DmaHeap()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}

DmaHeap::DmaHeap(DmaHeap&& other) noexcept : fd_(std::exchange(other.fd_, -1))
{
}

DmaHeap& DmaHeap::operator=(DmaHeap&& other) noexcept
{
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

int DmaHeap::Open(const char* node, DmaHeap& result) noexcept
{
    constexpr char PREFIX[] = "/dev/dma_heap/";
    constexpr size_t PREFIX_LENGTH = sizeof(PREFIX) - 1;
    if (node == nullptr || std::strncmp(node, PREFIX, PREFIX_LENGTH) != 0) {
        return -EINVAL;
    }
    const char* name = node + PREFIX_LENGTH;
    if (*name == '\0' || std::strchr(name, '/') != nullptr || std::strcmp(name, ".") == 0 ||
        std::strcmp(name, "..") == 0) {
        return -EINVAL;
    }
    if (result.fd_ >= 0) {
        return -EBUSY;
    }
    int fd;
    do {
        fd = open(node, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return -errno;
    }
    struct stat info {};
    int status;
    do {
        status = fstat(fd, &info);
    } while (status < 0 && errno == EINTR);
    int error = status < 0 ? errno : ENODEV;
    if (status < 0 || !S_ISCHR(info.st_mode)) {
        close(fd);
        return -error;
    }
    result.fd_ = fd;
    return 0;
}

int DmaHeap::Allocate(uint64_t bytes, DmaBuffer& result) const noexcept
{
    if (fd_ < 0 || bytes == 0) {
        return -EINVAL;
    }
    if (result.fd_ >= 0) {
        return -EBUSY;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return -ENOSYS;
    }
    uint64_t limit = static_cast<uint64_t>(std::numeric_limits<off_t>::max());
    if (std::numeric_limits<size_t>::max() < limit) {
        limit = std::numeric_limits<size_t>::max();
    }
    if (bytes > limit - (static_cast<uint64_t>(page) - 1)) {
        return -EOVERFLOW;
    }
    uint64_t size = ((bytes + page - 1) / page) * page;
    dma_heap_allocation_data request {};
    request.len = size;
    request.fd = std::numeric_limits<uint32_t>::max();
    request.fd_flags = O_RDWR | O_CLOEXEC;
    int status;
    do {
        status = ioctl(fd_, DMA_HEAP_IOCTL_ALLOC, &request);
    } while (status < 0 && errno == EINTR);
    if (status < 0) {
        return -errno;
    }
    if (request.fd > static_cast<uint32_t>(INT_MAX)) {
        return -EPROTO;
    }
    int fd = static_cast<int>(request.fd);
    off_t capacity = lseek(fd, 0, SEEK_END);
    int error = capacity < 0 ? errno : EPROTO;
    if (capacity < 0 || static_cast<uint64_t>(capacity) != size || lseek(fd, 0, SEEK_SET) < 0) {
        if (capacity >= 0 && static_cast<uint64_t>(capacity) == size) {
            error = errno;
        }
        close(fd);
        return -error;
    }
    result.fd_ = fd;
    result.size_ = size;
    return 0;
}

} // namespace OHOS::Qcom
