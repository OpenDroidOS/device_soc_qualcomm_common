/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcom/buffer_metadata.h"
#include "qcom/native_buffer_export.h"
#include "buffer_cpu_access.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <memory>
#include <new>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace OHOS::Qcom {
namespace {
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Native metadata uses little-endian words");
constexpr uint32_t MAGIC = 0x54444d51;
constexpr uint32_t VERSION = 1;
constexpr uint32_t CAPACITY = 64 * 1024;
enum Word { TAG, ABI, CAP, BYTES, COUNT, PHASE, ACCESS, OWNER, COOKIE_LO, COOKIE_HI, WRITING,
            SESSION_LO, SESSION_HI, WORDS };
enum Phase { IDLE, STARTING, ACTIVE, ENDING };
using Header = std::array<uint32_t, WORDS>;
constexpr uint32_t HEADER_BYTES = sizeof(Header);
constexpr int SEALS = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

int Duplicate(int fd) noexcept
{
    int result;
    do { result = fcntl(fd, F_DUPFD_CLOEXEC, 0); } while (result < 0 && errno == EINTR);
    return result < 0 ? -errno : result;
}

int Read(int fd, void* data, size_t bytes, off_t offset) noexcept
{
    auto* out = static_cast<uint8_t*>(data);
    while (bytes) {
        ssize_t count = pread(fd, out, bytes, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -errno : -EBADMSG;
        out += count; bytes -= count; offset += count;
    }
    return 0;
}

int Write(int fd, const void* data, size_t bytes, off_t offset) noexcept
{
    const auto* in = static_cast<const uint8_t*>(data);
    while (bytes) {
        ssize_t count = pwrite(fd, in, bytes, offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -errno : -EIO;
        in += count; bytes -= count; offset += count;
    }
    return 0;
}

uint32_t ReadWord(const uint8_t* data) noexcept
{
    uint32_t value; std::memcpy(&value, data, sizeof(value)); return value;
}

class File final {
public:
    ~File() { if (fd >= 0) close(fd); }
    int fd = -1;
    DmaBuffer pixels;
    Header header {};

    int Open(const BufferHandle& handle) noexcept
    {
        QcomBufferLayout layout;
        if (handle.reserveFds != 1 || qcom_buffer_read_layout(&handle, &layout) != 0)
            return -EINVAL;
        if (BufferGetCpuAccessMode(handle.reserve, handle.reserveFds, handle.reserveInts) != BUFFER_CPU_ACCESS_MANAGED)
            return -EINVAL;
        int error = DmaBuffer::Duplicate(handle.fd, pixels);
        if (error != 0) return error;
        if (pixels.Size() != layout.allocation_size) return -EINVAL;
        int retained = Duplicate(handle.reserve[0]);
        if (retained < 0) return retained;
        struct stat before {};
        if (fstat(retained, &before) < 0) { error = -errno; close(retained); return error; }
        if (!S_ISREG(before.st_mode) || before.st_size != CAPACITY) { close(retained); return -EBADMSG; }
        // Each operation needs a distinct open-file description: dup'd OFD locks share an owner.
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", retained);
        do { fd = open(path, O_RDWR | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
        error = fd < 0 ? -errno : 0;
        close(retained);
        if (error != 0) return error;
        struct stat after {};
        if (fstat(fd, &after) < 0) return -errno;
        if (after.st_dev != before.st_dev || after.st_ino != before.st_ino || after.st_size != CAPACITY)
            return -EBADMSG;
        int seals = fcntl(fd, F_GET_SEALS);
        if (seals < 0) return -errno;
        if ((seals & SEALS) != SEALS || (seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) return -EBADMSG;
        struct flock lock {};
        lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
        int result;
        do { result = fcntl(fd, F_OFD_SETLK, &lock); } while (result < 0 && errno == EINTR);
        if (result < 0) return errno == EACCES || errno == EAGAIN ? -EAGAIN : -errno;
        error = Read(fd, header.data(), HEADER_BYTES, 0);
        if (error != 0) return error;
        const int32_t* words = qcom_buffer_layout_words(&handle);
        uint64_t cookie = qcom_buffer_read_u64(words + QCOM_BUFFER_LAYOUT_WORDS + 4);
        uint64_t stored = header[COOKIE_LO] | (uint64_t(header[COOKIE_HI]) << 32);
        if (header[TAG] != MAGIC || header[ABI] != VERSION || header[CAP] != CAPACITY || stored != cookie ||
            header[BYTES] > CAPACITY - HEADER_BYTES || header[COUNT] > header[BYTES] / 8 ||
            header[PHASE] > ENDING || header[ACCESS] > DMA_BUF_SYNC_RW ||
            (header[PHASE] == IDLE && (header[ACCESS] != 0 || header[OWNER] != 0)) ||
            (header[PHASE] != IDLE && (header[ACCESS] == 0 || header[OWNER] == 0 ||
                                     (header[SESSION_LO] == 0 && header[SESSION_HI] == 0))))
            return -EBADMSG;
        return header[WRITING] == 0 ? 0 : -EOWNERDEAD;
    }

    int Store() noexcept { return Write(fd, header.data(), HEADER_BYTES, 0); }
};

struct Payload {
    std::unique_ptr<uint8_t[]> bytes;
    uint32_t size = 0;
    int Load(File& file) noexcept
    {
        size = file.header[BYTES];
        if (size) {
            bytes.reset(new (std::nothrow) uint8_t[size]);
            if (!bytes) return -ENOMEM;
            int error = Read(file.fd, bytes.get(), size, HEADER_BYTES);
            if (error != 0) return error;
        }
        uint32_t offset = 0, count = 0, previous = 0;
        while (offset < size) {
            if (size - offset < 8) return -EBADMSG;
            uint32_t key = ReadWord(bytes.get() + offset), length = ReadWord(bytes.get() + offset + 4);
            if ((count != 0 && key <= previous) || key == BUFFER_METADATA_CPU_ACCESS || length > size - offset - 8)
                return -EBADMSG;
            previous = key; count++; offset += 8 + length;
        }
        return count == file.header[COUNT] ? 0 : -EBADMSG;
    }
};

int Change(const BufferHandle& handle, uint32_t key, const uint8_t* value, size_t length, bool erase) noexcept
{
    if (key == BUFFER_METADATA_CPU_ACCESS) return -EACCES;
    if (length > CAPACITY - HEADER_BYTES - 8) return -E2BIG;
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    Payload payload;
    error = payload.Load(file);
    if (error != 0) return error;
    uint32_t start = 0, end = 0;
    bool found = false;
    while (start < payload.size) {
        uint32_t storedKey = ReadWord(payload.bytes.get() + start);
        uint32_t storedLength = ReadWord(payload.bytes.get() + start + 4);
        if (storedKey >= key) {
            found = storedKey == key;
            break;
        }
        start += 8 + storedLength;
    }
    end = found ? start + 8 + ReadWord(payload.bytes.get() + start + 4) : start;
    if (erase && !found) return -ENODATA;
    size_t inserted = erase ? 0 : 8 + length;
    size_t size = payload.size - (end - start) + inserted;
    if (size > CAPACITY - HEADER_BYTES) return -ENOSPC;
    std::unique_ptr<uint8_t[]> updated(size ? new (std::nothrow) uint8_t[size] : nullptr);
    if (size && !updated) return -ENOMEM;
    if (start) std::memcpy(updated.get(), payload.bytes.get(), start);
    if (!erase) {
        uint32_t storedLength = static_cast<uint32_t>(length);
        std::memcpy(updated.get() + start, &key, 4);
        std::memcpy(updated.get() + start + 4, &storedLength, 4);
        if (length) std::memcpy(updated.get() + start + 8, value, length);
    }
    if (payload.size > end) std::memcpy(updated.get() + start + inserted, payload.bytes.get() + end, payload.size - end);
    file.header[WRITING] = 1;
    error = file.Store();
    if (error != 0) return error;
    error = Write(file.fd, updated.get(), size, HEADER_BYTES);
    if (error != 0) return error;
    file.header[BYTES] = static_cast<uint32_t>(size);
    file.header[COUNT] = file.header[COUNT] + (found ? 0 : 1) - (erase ? 1 : 0);
    file.header[WRITING] = 0;
    return file.Store();
}

int Synchronize(int fd, uint32_t flags) noexcept
{
    dma_buf_sync operation {flags};
    int result;
    do { result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &operation); }
    while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result < 0 ? -errno : 0;
}
} // namespace

int BufferMetadata::Attach(BufferHandle*& handle) noexcept
{
    QcomBufferLayout layout;
    if (!handle || handle->reserveFds != 0 || handle->virAddr != nullptr || qcom_buffer_read_layout(handle, &layout) != 0)
        return -EINVAL;
    uint64_t cookie = 0;
    uint8_t* out = reinterpret_cast<uint8_t*>(&cookie);
    size_t remaining = sizeof(cookie);
    while (remaining) {
        ssize_t count = getrandom(out, remaining, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return count < 0 ? -errno : -EIO;
        out += count; remaining -= count;
    }
    if (cookie == 0) return -EAGAIN;
    int metadata = memfd_create("native-buffer-metadata", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (metadata < 0) return -errno;
    Header header {};
    header[TAG] = MAGIC; header[ABI] = VERSION; header[CAP] = CAPACITY;
    header[COOKIE_LO] = static_cast<uint32_t>(cookie); header[COOKIE_HI] = static_cast<uint32_t>(cookie >> 32);
    int error = ftruncate(metadata, CAPACITY) < 0 ? -errno : Write(metadata, header.data(), HEADER_BYTES, 0);
    if (error == 0 && fcntl(metadata, F_ADD_SEALS, SEALS) < 0) error = -errno;
    if (error != 0) { close(metadata); return error; }
    auto* extended = static_cast<BufferHandle*>(std::calloc(1,
        sizeof(BufferHandle) + (1 + QCOM_BUFFER_METADATA_WORDS) * sizeof(int32_t)));
    if (!extended) { close(metadata); return -ENOMEM; }
    *extended = *handle;
    extended->fd = Duplicate(handle->fd);
    if (extended->fd < 0) { error = extended->fd; close(metadata); std::free(extended); return error; }
    extended->reserveFds = 1; extended->reserveInts = QCOM_BUFFER_METADATA_WORDS;
    extended->reserve[0] = metadata;
    if (BufferSetCpuAccessTrailer(extended->reserve, extended->reserveFds, extended->reserveInts, 0, cookie) != 0) {
        FreeNativeBuffer(extended);
        return -EINVAL;
    }
    error = qcom_buffer_write_layout(extended, &layout);
    if (error != 0) { FreeNativeBuffer(extended); return error; }
    FreeNativeBuffer(handle);
    handle = extended;
    return 0;
}

int BufferMetadata::Clone(const BufferHandle& input, BufferHandle*& output) noexcept
{
    if (output != nullptr) return -EBUSY;
    int error = Validate(input);
    if (error != 0) return error;
    const size_t words = input.reserveFds + input.reserveInts;
    auto* result = static_cast<BufferHandle*>(std::calloc(1, sizeof(BufferHandle) + words * sizeof(int32_t)));
    if (result == nullptr) return -ENOMEM;
    *result = input;
    result->virAddr = nullptr;
    result->phyAddr = 0;
    result->fd = -1;
    std::memcpy(result->reserve, input.reserve, words * sizeof(int32_t));
    result->reserve[0] = -1;
    result->fd = Duplicate(input.fd);
    if (result->fd < 0) { error = result->fd; FreeNativeBuffer(result); return error; }
    result->reserve[0] = Duplicate(input.reserve[0]);
    if (result->reserve[0] < 0) { error = result->reserve[0]; FreeNativeBuffer(result); return error; }
    error = Validate(*result);
    if (error != 0) { FreeNativeBuffer(result); return error; }
    output = result;
    return 0;
}

int BufferMetadata::Validate(const BufferHandle& handle) noexcept
{
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    Payload payload;
    return payload.Load(file);
}

int BufferMetadata::Set(const BufferHandle& handle, uint32_t key, const std::vector<uint8_t>& value) noexcept
{
    return Change(handle, key, value.data(), value.size(), false);
}

int BufferMetadata::Erase(const BufferHandle& handle, uint32_t key) noexcept
{
    return Change(handle, key, nullptr, 0, true);
}

int BufferMetadata::Get(const BufferHandle& handle, uint32_t key, std::vector<uint8_t>& value) noexcept
{
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    if (key == BUFFER_METADATA_CPU_ACCESS) {
        value.assign(BUFFER_CPU_ACCESS_PROTOCOL, BUFFER_CPU_ACCESS_PROTOCOL + sizeof(BUFFER_CPU_ACCESS_PROTOCOL));
        return 0;
    }
    Payload payload;
    error = payload.Load(file);
    if (error != 0) return error;
    for (uint32_t offset = 0; offset < payload.size;) {
        uint32_t stored = ReadWord(payload.bytes.get() + offset), length = ReadWord(payload.bytes.get() + offset + 4);
        if (stored == key) {
            value.assign(payload.bytes.get() + offset + 8, payload.bytes.get() + offset + 8 + length);
            return 0;
        }
        offset += 8 + length;
    }
    return -ENODATA;
}

int BufferMetadata::List(const BufferHandle& handle, std::vector<uint32_t>& keys) noexcept
{
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    Payload payload;
    error = payload.Load(file);
    if (error != 0) return error;
    std::vector<uint32_t> result;
    for (uint32_t offset = 0; offset < payload.size;) {
        result.push_back(ReadWord(payload.bytes.get() + offset));
        offset += 8 + ReadWord(payload.bytes.get() + offset + 4);
    }
    result.push_back(BUFFER_METADATA_CPU_ACCESS);
    keys.swap(result);
    return 0;
}

int BufferMetadata::BeginCpuAccess(const BufferHandle& handle, uint64_t* session) noexcept
{
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    uint32_t access = handle.usage & DMA_BUF_SYNC_RW;
    if (access == 0) access = DMA_BUF_SYNC_RW;
    uint64_t current = file.header[SESSION_LO] | (uint64_t(file.header[SESSION_HI]) << 32);
    if (file.header[PHASE] == ACTIVE) {
        if (file.header[OWNER] != static_cast<uint32_t>(getpid()) || file.header[ACCESS] != access) return -EBUSY;
        if (session) *session = current;
        return 0;
    }
    if (file.header[PHASE] != IDLE) return -EOWNERDEAD;
    if (current == UINT64_MAX) return -EOVERFLOW;
    current++;
    file.header[SESSION_LO] = static_cast<uint32_t>(current);
    file.header[SESSION_HI] = static_cast<uint32_t>(current >> 32);
    file.header[PHASE] = STARTING; file.header[OWNER] = getpid(); file.header[ACCESS] = access;
    error = file.Store();
    if (error != 0) return error;
    error = Synchronize(file.pixels.Fd(), access | DMA_BUF_SYNC_START);
    file.header[PHASE] = error == 0 ? ACTIVE : IDLE;
    if (error != 0) { file.header[OWNER] = 0; file.header[ACCESS] = 0; }
    int stored = file.Store();
    if (error == 0 && stored == 0 && session) *session = current;
    return error != 0 ? error : stored;
}

int BufferMetadata::EndCpuAccess(const BufferHandle& handle, bool ownerOnly, uint64_t expectedSession) noexcept
{
    File file;
    int error = file.Open(handle);
    if (error != 0) return error;
    uint64_t current = file.header[SESSION_LO] | (uint64_t(file.header[SESSION_HI]) << 32);
    if (expectedSession != 0 && current != expectedSession) return 0;
    if (file.header[PHASE] == IDLE) return 0;
    if (file.header[PHASE] != ACTIVE) return -EOWNERDEAD;
    if (ownerOnly && file.header[OWNER] != static_cast<uint32_t>(getpid())) return 0;
    file.header[PHASE] = ENDING;
    error = file.Store();
    if (error != 0) return error;
    error = Synchronize(file.pixels.Fd(), file.header[ACCESS] | DMA_BUF_SYNC_END);
    file.header[PHASE] = error == 0 ? IDLE : ACTIVE;
    if (error == 0) { file.header[OWNER] = 0; file.header[ACCESS] = 0; }
    int stored = file.Store();
    return error != 0 ? error : stored;
}

} // namespace OHOS::Qcom
