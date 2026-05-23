// libui_n_era_shim.cpp
// Provides 2 N-era (Android N / API 25) GraphicBuffer symbols that
// libcam_utils.so (direct dep of camera.mt8163.so) imports from libui.so,
// but that were removed/replaced in Android 11's libui.so.
//
// Produced (N-era, what blobs call -- byte-exact per g1_symbol_closure.yaml):
//   _ZN7android13GraphicBuffer4lockEjPPv
//     android::GraphicBuffer::lock(unsigned int, void**)
//   _ZN7android13GraphicBufferC1EjjijjP13native_handleb
//     android::GraphicBuffer::GraphicBuffer(uint, uint, int, uint, uint,
//                                           native_handle*, bool)
//
// Imported (Android 11, resolved from device libui.so at load time):
//   _ZN7android13GraphicBuffer4lockEjPPvPiS3_
//     android::GraphicBuffer::lock(uint, void**, int*, int*)
//   _ZN7android13GraphicBufferC1EjjijjjP13native_handleb
//     android::GraphicBuffer::GraphicBuffer(uint, uint, int, uint, uint,
//                                           uint /*layerCount*/, native_handle*, bool)
//
// Forwarding strategy:
//   lock:  pass nullptr for outBytesPerPixel and outBytesPerStride (allowed by Android 11 contract)
//   ctor:  insert layerCount=1 (correct for all 2D camera buffers)
//
// native_handle is a global struct (not in android:: namespace) -- mangled as
// P13native_handle. Must be declared at global scope to match libui.so's exports.

#include <stdint.h>

// native_handle is in the global namespace in Android's C headers.
// Forward-declare it here so android::GraphicBuffer methods can reference it.
struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
};

namespace android {

// Only declare the methods/ctors we need to produce and consume.
// The actual GraphicBuffer class body lives in libui.so.
class GraphicBuffer {
public:
    // N-era exports (what this shim produces):
    int lock(uint32_t usage, void** vaddr);
    GraphicBuffer(uint32_t w, uint32_t h, int format,
                  uint32_t rUsage, uint32_t pUsage,
                  ::native_handle* handle, bool keepOwnership);

    // Android-11 targets (resolved from libui.so at load time):
    int lock(uint32_t usage, void** vaddr,
             int* outBytesPerPixel, int* outBytesPerStride);
    GraphicBuffer(uint32_t w, uint32_t h, int format,
                  uint32_t layerCount,
                  uint32_t rUsage, uint32_t pUsage,
                  ::native_handle* handle, bool keepOwnership);
};

// N-era 2-arg lock -> Android-11 5-arg lock
int GraphicBuffer::lock(uint32_t usage, void** vaddr) {
    return lock(usage, vaddr, nullptr, nullptr);
}

// N-era 7-arg ctor -> Android-11 8-arg ctor (layerCount=1)
GraphicBuffer::GraphicBuffer(uint32_t w, uint32_t h, int format,
                             uint32_t rUsage, uint32_t pUsage,
                             ::native_handle* handle, bool keepOwnership)
    : GraphicBuffer(w, h, format, /*layerCount=*/1u,
                    rUsage, pUsage, handle, keepOwnership) {}

// --- GraphicBufferMapper N-era 4-arg lock -> A11 6-arg lock ---
// N-era: _ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv
//        GraphicBufferMapper::lock(native_handle const*, uint, Rect const&, void**)
// A11:   _ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_
//        ...adds int* outBytesPerPixel, int* outBytesPerStride

struct Rect;  // opaque

class GraphicBufferMapper {
public:
    // N-era export (what this shim produces):
    int lock(::native_handle const* handle, uint32_t usage,
             Rect const& bounds, void** vaddr);
    // A11 target (resolved from libui.so at load time):
    int lock(::native_handle const* handle, uint32_t usage,
             Rect const& bounds, void** vaddr,
             int* outBytesPerPixel, int* outBytesPerStride);
};

int GraphicBufferMapper::lock(::native_handle const* handle, uint32_t usage,
                              Rect const& bounds, void** vaddr) {
    return lock(handle, usage, bounds, vaddr, nullptr, nullptr);
}

}  // namespace android
