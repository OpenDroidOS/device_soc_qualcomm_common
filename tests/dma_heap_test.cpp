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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using OHOS::Qcom::CpuAccess;
using OHOS::Qcom::CpuMapping;
using OHOS::Qcom::DmaBuffer;
using OHOS::Qcom::DmaHeap;

namespace {
constexpr const char* TEST_HEAP = "/dev/dma_heap/qcom,unit-test";
std::deque<int> allocationErrors;
std::deque<int> syncErrors;
std::set<std::pair<dev_t, ino_t>> testBuffers;
std::vector<uint64_t> syncFlags;
bool invalidResultFd = false;
bool wrongCapacity = false;
int heapOpenCount = 0;

void Check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

int Inject(std::deque<int>& errors)
{
    if (errors.empty()) {
        return 0;
    }
    errno = errors.front();
    errors.pop_front();
    return -1;
}

int FdCount()
{
    DIR* directory = opendir("/proc/self/fd");
    CHECK(directory != nullptr);
    int count = 0;
    while (dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

DmaHeap Heap()
{
    DmaHeap heap;
    CHECK(DmaHeap::Open(TEST_HEAP, heap) == 0);
    return heap;
}

DmaBuffer Buffer(DmaHeap& heap, uint64_t bytes = 17)
{
    DmaBuffer buffer;
    CHECK(heap.Allocate(bytes, buffer) == 0);
    return buffer;
}

void ExplicitHeapOnly()
{
    DmaHeap heap;
    const char* invalidPaths[] = {nullptr, "", "/tmp/system", "/dev/dma_heap/", "/dev/dma_heap/..",
        "/dev/dma_heap/system/other"};
    for (const char* path : invalidPaths) {
        CHECK(DmaHeap::Open(path, heap) == -EINVAL);
    }
    CHECK(!heap.IsOpen());
    CHECK(heapOpenCount == 0);
    CHECK(DmaHeap::Open("/dev/dma_heap/no-such-unit-heap", heap) == -ENOENT);
    CHECK(heapOpenCount == 1);
    CHECK(DmaHeap::Open("/dev/dma_heap/unit-regular-file", heap) == -ENODEV);
    CHECK(!heap.IsOpen());
}

void AllocationAndCloexec()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    CHECK(buffer.Size() == static_cast<uint64_t>(sysconf(_SC_PAGESIZE)));
    CHECK((fcntl(buffer.Fd(), F_GETFD) & FD_CLOEXEC) != 0);
    int transfer = buffer.DuplicateFd();
    CHECK(transfer >= 0 && transfer != buffer.Fd());
    CHECK((fcntl(transfer, F_GETFD) & FD_CLOEXEC) != 0);
    close(transfer);
}

void InvalidSizesAndBusyOutputs()
{
    DmaHeap heap = Heap();
    DmaBuffer empty;
    CHECK(heap.Allocate(0, empty) == -EINVAL);
    CHECK(heap.Allocate(std::numeric_limits<uint64_t>::max(), empty) == -EOVERFLOW);
    CHECK(empty.Fd() == -1 && empty.Size() == 0);
    DmaBuffer allocated = Buffer(heap);
    int fd = allocated.Fd();
    CHECK(heap.Allocate(4096, allocated) == -EBUSY);
    CHECK(DmaBuffer::Duplicate(fd, allocated) == -EBUSY);
    CHECK(DmaHeap::Open(TEST_HEAP, heap) == -EBUSY);
    CHECK(allocated.Fd() == fd);
}

void AllocationErrors()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer;
    allocationErrors = {ENOMEM};
    CHECK(heap.Allocate(4096, buffer) == -ENOMEM);
    CHECK(buffer.Fd() == -1 && buffer.Size() == 0);
    allocationErrors = {EINTR};
    CHECK(heap.Allocate(4096, buffer) == 0);
    CHECK(allocationErrors.empty());
}

void InvalidAllocationResult()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer;
    invalidResultFd = true;
    CHECK(heap.Allocate(4096, buffer) == -EPROTO);
    invalidResultFd = false;
    wrongCapacity = true;
    CHECK(heap.Allocate(4096, buffer) == -EPROTO);
    CHECK(buffer.Fd() == -1 && buffer.Size() == 0);
}

void CpuSessionsAndImport()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    CpuMapping map;
    CHECK(buffer.Map(CpuAccess::READ_WRITE, map) == 0);
    CHECK(map.IsActive() && map.Data() != nullptr);
    std::memcpy(map.Data(), "DMA", 4);
    CHECK(map.Begin() == -EBUSY);
    CHECK(map.End() == 0 && !map.IsActive() && map.Data() == nullptr);
    CHECK(map.End() == -EINVAL);
    CHECK(map.Begin() == 0);
    CHECK(std::memcmp(map.Data(), "DMA", 4) == 0);
    CHECK(map.Finish() == 0 && map.Size() == 0 && map.Data() == nullptr);
    CHECK(map.Finish() == 0);
    DmaBuffer imported;
    CHECK(DmaBuffer::Duplicate(buffer.Fd(), imported) == 0);
    CHECK(imported.Size() == buffer.Size());
    CHECK((fcntl(imported.Fd(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(imported.Map(CpuAccess::READ, map) == 0);
    CHECK(std::memcmp(map.Data(), "DMA", 4) == 0);
    CHECK(map.Finish() == 0);
    CHECK(syncFlags == std::vector<uint64_t>({DMA_BUF_SYNC_RW | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END, DMA_BUF_SYNC_RW | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END, DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END}));
}

void MappingOwnsItsDescriptor()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    CpuMapping original;
    CHECK(buffer.Map(CpuAccess::WRITE, original) == 0);
    CpuMapping moved(std::move(original));
    CHECK(original.Data() == nullptr && original.Size() == 0);
    buffer = DmaBuffer();
    std::memset(moved.Data(), 42, moved.Size());
    CHECK(moved.Finish() == 0);
}

void CacheRetryAndFailedEnd()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    CpuMapping map;
    syncErrors = {EINTR, EAGAIN};
    CHECK(buffer.Map(CpuAccess::READ_WRITE, map) == 0);
    CHECK(syncFlags.size() == 3 && syncErrors.empty());
    syncErrors = {EIO};
    CHECK(map.Finish() == -EIO);
    CHECK(map.IsActive() && map.Data() != nullptr && map.Size() != 0);
    CHECK(map.Finish() == 0);
    CHECK(syncFlags.size() == 5 && syncFlags[3] == (DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END));
}

void FailedStartDoesNotExposeMemory()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    CpuMapping map;
    int count = FdCount();
    syncErrors = {EACCES};
    CHECK(buffer.Map(CpuAccess::READ, map) == -EACCES);
    CHECK(!map.IsActive() && map.Data() == nullptr && map.Size() == 0);
    CHECK(FdCount() == count);
}

void OrdinaryFileIsNotCpuDmaAccess()
{
    int fd = memfd_create("ordinary-file", MFD_CLOEXEC);
    CHECK(fd >= 0 && ftruncate(fd, 4096) == 0);
    DmaBuffer imported;
    CHECK(DmaBuffer::Duplicate(fd, imported) == 0);
    CpuMapping map;
    CHECK(imported.Map(CpuAccess::READ, map) == -ENOTTY);
    CHECK(map.Data() == nullptr);
    close(fd);
    int pipeFds[2];
    CHECK(pipe(pipeFds) == 0);
    DmaBuffer invalid;
    CHECK(DmaBuffer::Duplicate(pipeFds[0], invalid) == -ESPIPE);
    close(pipeFds[0]);
    close(pipeFds[1]);
}

void ReadOnlyImportCannotMapForWrite()
{
    DmaHeap heap = Heap();
    DmaBuffer buffer = Buffer(heap);
    std::string path = "/proc/self/fd/" + std::to_string(buffer.Fd());
    int readOnly = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(readOnly >= 0);
    DmaBuffer imported;
    CHECK(DmaBuffer::Duplicate(readOnly, imported) == 0);
    close(readOnly);
    CpuMapping map;
    int count = FdCount();
    CHECK(imported.Map(CpuAccess::WRITE, map) == -EACCES);
    CHECK(map.Data() == nullptr && map.Size() == 0 && !map.IsActive());
    CHECK(FdCount() == count && syncFlags.empty());
    CHECK(imported.Map(CpuAccess::READ, map) == 0);
    CHECK(map.Finish() == 0);
}

void MovesAndInvalidAccess()
{
    DmaHeap old = Heap();
    DmaHeap heap(std::move(old));
    DmaBuffer empty;
    CHECK(!old.IsOpen() && old.Allocate(4096, empty) == -EINVAL);
    DmaBuffer buffer = Buffer(heap);
    DmaBuffer moved(std::move(buffer));
    CHECK(buffer.Fd() == -1 && buffer.Size() == 0);
    CpuMapping map;
    CHECK(moved.Map(static_cast<CpuAccess>(0), map) == -EINVAL);
    CHECK(moved.Map(static_cast<CpuAccess>(8), map) == -EINVAL);
    CHECK(buffer.Map(CpuAccess::READ, map) == -EINVAL);
    CHECK(moved.Map(CpuAccess::READ, map) == 0);
    CHECK(moved.Map(CpuAccess::READ, map) == -EBUSY);
    CHECK(map.Finish() == 0);
    CHECK(DmaBuffer::Duplicate(-1, empty) == -EBADF);
}

void DescriptorZeroIsValid()
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(STDIN_FILENO);
        {
            DmaHeap heap = Heap(); // The heap occupies fd zero.
            CHECK(heap.IsOpen());
            DmaBuffer buffer = Buffer(heap);
            CHECK(buffer.Fd() >= 0);
        }
        {
            DmaHeap heap = Heap();
            DmaBuffer buffer = Buffer(heap);
            heap = DmaHeap(); // Release the heap's fd zero before exporting.
            int zero = buffer.DuplicateFd();
            CHECK(zero == 0);
            DmaBuffer imported;
            CHECK(DmaBuffer::Duplicate(zero, imported) == 0);
            close(zero);
        }
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
} // namespace

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_open(const char* path, int flags, ...)
{
    if (std::strncmp(path, "/dev/dma_heap/", 14) == 0) {
        ++heapOpenCount;
        CHECK(flags == (O_RDONLY | O_CLOEXEC));
        if (std::strcmp(path, TEST_HEAP) == 0) {
            return __real_open("/dev/null", flags);
        }
        if (std::strcmp(path, "/dev/dma_heap/unit-regular-file") == 0) {
            return memfd_create("not-a-heap", MFD_CLOEXEC);
        }
        errno = ENOENT;
        return -1;
    }
    return __real_open(path, flags);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    void* data = va_arg(args, void*);
    va_end(args);
    if (request == DMA_HEAP_IOCTL_ALLOC) {
        auto* allocation = static_cast<dma_heap_allocation_data*>(data);
        CHECK(allocation->fd_flags == (O_RDWR | O_CLOEXEC) && allocation->heap_flags == 0);
        if (Inject(allocationErrors) < 0) {
            return -1;
        }
        if (invalidResultFd) {
            return 0;
        }
        int buffer = memfd_create("unit-dma-buf", MFD_CLOEXEC);
        CHECK(buffer >= 0 && ftruncate(buffer, allocation->len - (wrongCapacity ? 1 : 0)) == 0);
        struct stat info {};
        CHECK(fstat(buffer, &info) == 0);
        testBuffers.emplace(info.st_dev, info.st_ino);
        allocation->fd = buffer;
        return 0;
    }
    if (request == DMA_BUF_IOCTL_SYNC) {
        struct stat info {};
        if (fstat(fd, &info) == 0 && testBuffers.count({info.st_dev, info.st_ino}) != 0) {
            syncFlags.push_back(static_cast<dma_buf_sync*>(data)->flags);
            return Inject(syncErrors);
        }
    }
    return __real_ioctl(fd, request, data);
}

int main()
{
    void (*tests[])() = {ExplicitHeapOnly, AllocationAndCloexec, InvalidSizesAndBusyOutputs,
        AllocationErrors, InvalidAllocationResult, CpuSessionsAndImport, MappingOwnsItsDescriptor,
        CacheRetryAndFailedEnd, FailedStartDoesNotExposeMemory, OrdinaryFileIsNotCpuDmaAccess,
        ReadOnlyImportCannotMapForWrite, MovesAndInvalidAccess, DescriptorZeroIsValid};
    for (auto test : tests) {
        allocationErrors.clear();
        syncErrors.clear();
        testBuffers.clear();
        syncFlags.clear();
        invalidResultFd = false;
        wrongCapacity = false;
        heapOpenCount = 0;
        int count = FdCount();
        test();
        CHECK(FdCount() == count);
    }
    std::printf("13 DMA-heap ownership/cache tests passed; no leaked descriptors\n");
}
