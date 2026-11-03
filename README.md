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
32-bit words. Layout-only exports use no reserved fds. Bounds,
format/stride agreement and non-overlapping plane regions are checked. Plane
regions include any metadata required by the modifier. Unrecognized handles
reject; there is no assumed linear layout for another allocator's buffers.

The allocator must derive the actual modifier, pitch, offsets and capacity from
its backend. Exporting a descriptor does not establish GPU or display capability.
Consumers must validate the layout against their hardware, check the actual fd
capacity and obey the existing fence/CPU-access ownership contract. The header
dependency needs the native HDI, native-buffer format and DRM UAPI include paths.

## Shared CPU ownership

`BufferMetadata::Attach` upgrades a new layout-only handle with a size-sealed
metadata fd and a versioned CPU-access trailer. `Clone` retains both descriptors
with close-on-exec and clears borrowed virtual/physical addresses. Release the
returned handle with `FreeNativeBuffer`. Metadata contains validated plain bytes;
locks use distinct open-file descriptions so operations also serialize between
processes. Metadata keys support bounded get/set/list/erase operations; the CPU
access capability key is read-only.

`BeginCpuAccess` starts a cache session after the caller has waited its acquire
fence. `EndCpuAccess` ends the shared session before device work or an ownership
transfer. Failed END leaves the session active for retry. The optional session
generation prevents stale local mapping cleanup from ending a later session.
Ending access does not wait fences or permit concurrent CPU/device use.

The display-buffer VDI keeps mapping separate from CPU access: `Mmap` only maps,
`InvalidateCache` begins access and `FlushCache` ends it. Mapping aliases retain
their own descriptors and share a session lease; unmapping an untouched alias
does not end another alias's access. Native Surface address access and queue
handoffs use the managed trailer to invoke these operations. Callers of native
mapping APIs must wait their own fences before requesting a CPU address.

The `display/buffer:buffer_metadata` target provides a complete static archive
for external renderers. Native Turnip imports retain metadata-backed handles for
the image lifetime and end CPU access before importing acquire synchronization.
An unsuccessful cache handoff closes the owned incoming fence and returns an
error without making the image available. Layout-only handles lack this shared
ownership contract and are rejected by that native image-import path.

## Vulkan allocation

Depend on `:vulkan_buffer_allocator` and initialize `VulkanBufferAllocator` with
the installed Vulkan loader library. It selects a device reporting Qualcomm's
vendor ID and Mesa's Turnip driver ID. Supply its 16-byte Vulkan device UUID when
more than one matches. Missing extensions, ambiguous selection and unsupported
formats/usages return an error; no GPU identity is inferred from a product name.

`IsSupported` checks the actual external-image capabilities, image requirements
and compatible memory types. `Allocate` creates a linear exportable image,
allocates dedicated memory and obtains the real plane offsets/pitches through
Vulkan. The exported DMA-BUF's actual capacity determines the handle capacity;
plane regions include the driver's padding. The returned owned handle uses
`FreeNativeBuffer`. Temporary Vulkan images and memory are released after export.

CPU read/write, DMA, cached CPU memory and GPU render/texture/composer usage bits
are accepted only when their queried format/memory requirements can be met.
Protected, assigned-size, camera/video and unknown vendor usage bits require
separate allocation contracts. This target provides allocation only. Mapping,
fence ownership, CPU cache sessions and display scanout belong to their providers;
allocation does not establish display-controller compatibility.

## Native product build

Register the common bundle through the product's native subsystem overlay and
select `hdf:qcom_display`. Its `display:display` group builds the VDI and shared
metadata archive. Set the component feature `qcom_display_gpu_kmd` to `kgsl` or
`msm` for the actual kernel interface to also build Turnip. The DRM selection
requires the native `libdrm` product component. This selection does not identify
a GPU model or establish kernel/firmware compatibility.

The renderer action builds Mesa's real OHOS sources using the selected product
libraries, native LLVM compiler, built native SDK sysroot and metadata archive.
Host Python needs Meson and Mako; a host `glslangValidator`, Ninja and pkg-config
must be available. `qcom_display_meson_python` and `qcom_display_glslang` select
explicit host tools when needed. Debug products retain Mesa debug assertions
and symbols; sanitizer/coverage configurations reject until their matching
compiler flags are implemented.

Meson output defaults to the product's GN output directory. An explicit
`qcom_display_gpu_build_dir` can reuse an existing native Turnip cache after its
compiler, target and sysroot are checked. Configuration and build failures
propagate; the action never wipes the cache. Builds use at most sixteen jobs.
The generated vendor ICD names `/vendor/lib64/libvulkan_freedreno.so` and is
checked against that installation path. The targets currently build without
installing the driver, ICD or VDI by default. Select the component feature
`qcom_display_native_buffer = true` to install the paired driver, ICD and VDI
and build the native Vulkan surface layer and its configuration. This also
enables Turnip's native HDI buffer import and fence interface. Other Turnip
builds leave that interface unadvertised.

The paired selection requires `drivers_peripheral_display_vdi_default = false`
on the existing display component, so allocator/mapper/metadata services load
the vendor VDI. Preserve that component's other inherited features when making
the override. It also requires the matching Surface CPU ownership and Vulkan
layer fence changes. Use the normal native Vulkan loader and layer; no layer
disable flag or direct ICD loading is needed. The loader discovers the generated
ICD under `/vendor/etc/vulkan/icd.d`. This buffer/renderer pair does not supply
a display composer, prove a GPU is supported, or replace kernel/firmware checks.

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
