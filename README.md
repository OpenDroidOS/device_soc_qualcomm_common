# Qualcomm native platform support

Shared Linux support for native OpenHarmony Qualcomm providers. SoC and board
configuration supplies the actual controller, memory and firmware selections.

## DMA-buffer memory

Depend on `//device/soc/qualcomm/common:dma_heap` and include
`qcom/dma_heap.h`. `DmaHeap::Open` requires an explicit `/dev/dma_heap/<name>`
character device. It never substitutes a different heap. Allocation checks page
rounding and backing capacity and requests close-on-exec descriptors atomically.
Objects are move-only; imports duplicate descriptors instead of consuming them.

`DmaBuffer::Map` maps memory and begins the selected CPU access. `CpuMapping::End`
ends it, and `Begin` starts another access on the persistent mapping. `Data` is
available only during an active access. Call `Finish` and check its return value
before handing the buffer to another consumer; failed END leaves the access
active so it can be retried. Destruction releases resources with best-effort END
and cannot report completion errors. A mapping owns its own descriptor and can
outlive the buffer object. Operations return zero or negative errno values;
`DuplicateFd` returns a new descriptor or negative errno. Descriptor zero is valid.

CPU cache synchronization does not wait acquire fences, establish mutual
exclusion or submit work to a device. Providers must serialize access and wait
the applicable fences before CPU access. Heap allocation alone establishes no
pixel layout, GPU import, scanout, protected-memory or video capability. The
provider must validate its own format, usage, layout and imported handle schema.
`Duplicate` checks seekable backing size; mapping additionally requires a working
DMA-BUF synchronization ioctl. No heap is registered or driver loaded by this
library. The GN target installs no display VDI implementation by itself.

See the Linux [DMA-heap API](https://docs.kernel.org/userspace-api/dma-buf-heaps.html)
and [DMA-buffer synchronization](https://docs.kernel.org/driver-api/dma-buf.html).

## Native buffer handles

Depend on `:native_buffer_export` and include `qcom/native_buffer_export.h` to
export a `DmaBuffer` as an owned native HDI `BufferHandle`. Supply the dimensions,
native format, usage and the layout calculated by the selected graphics backend.
`ExportNativeBuffer` duplicates the fd; `FreeNativeBuffer` releases that returned
handle. The backing buffer keeps its own descriptor. No physical address or CPU
mapping is attached to the exported handle.

`qcom/native_buffer.h` provides C-compatible layout readers for consumers. The
handle uses the native HDI definition, not an incompatible public SDK window
header. Its reserved integers contain a producer tag and layout version, DRM
fourcc/modifier, allocation size and up to four planes in DRM format order.
Offsets, row pitches and plane sizes are bytes; each 64-bit value uses low/high
32-bit words. The transport uses one backing fd and no reserved fds. Bounds,
format/stride agreement and non-overlapping plane regions are checked. Plane
regions include any metadata required by the modifier. Unrecognized handles
reject; there is no assumed linear layout for another allocator's buffers.

The allocator must derive the actual modifier, pitch, offsets and capacity from
its backend. Exporting a descriptor does not establish GPU or display capability.
Consumers must validate the layout against their hardware, check the actual fd
capacity and obey the existing fence/CPU-access ownership contract. The header
dependency needs the native HDI, native-buffer format and DRM UAPI include paths.

## Host checks

Run outside the source tree with an explicit output path:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude display/memory/dma_heap.cpp \
  tests/dma_heap_test.cpp -Wl,--wrap=open -Wl,--wrap=ioctl \
  -o /path/to/dma_heap_test
/path/to/dma_heap_test
```

The test executable substitutes heap allocation and synchronization ioctls with
memfd fixtures. It checks ownership, errors and access sequencing without a DMA
device. These checks do not establish hardware allocation or device compatibility.
