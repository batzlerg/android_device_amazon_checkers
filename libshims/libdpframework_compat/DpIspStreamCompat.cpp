// libdpframework_compat -- DpIspStream ABI shadow-allocation compat shim.
//
// Shadow-allocates A11-sized DpIspStream buffers to bridge the struct-size
// growth from 0x5a8 (N-era) to ~0x46f8 (A11) in the MTK camera blob closure.
//
// PROBLEM
// =======
// N-era proprietary MediaTek camera blobs (libcam.campipe.so,
// libcam.camadapter.so, libimageio_plat_drv{,_FrmB}.so, libcam.iopipe.so)
// were compiled against a DpIspStream class sized 0x5a8 bytes (~1.4 KB).
// A11 libdpframework.so ships a DpIspStream that writes fields at offsets
// up to ~0x46f8 (~18 KB) -- a ~13x struct-size growth. The A11 ctor
// overruns the caller's undersized allocation and SIGSEGV_ACCERRs at
// this+0x411c when it hits the adjacent read-only page.
//
// SOLUTION -- SHADOW-ALLOCATION SHIM
// ============================================
// This shim exports every DpIspStream symbol the caller blobs use
// (17 symbols total: ctor, dtor, 15 methods). At construction:
//   1. The caller's operator new(0x5a8) returns a pointer the caller
//      treats as a DpIspStream*. This pointer is the "facade" pointer.
//   2. The shim's ctor allocates a SECOND, larger buffer (0x4800 bytes,
//      18 KB) from the heap. This is the "real" A11-sized object.
//   3. The shim dlsym's the real A11 ctor from /vendor/lib/libdpframework.so
//      and invokes it on the real buffer.
//   4. A shadow map (facade_ptr -> real_ptr) is updated under a mutex.
// Subsequent method calls receive the facade pointer as `this`; the shim
// looks up the real pointer and delegates to the A11 method.
// At destruction, the shim reverses: dlsym the real dtor, invoke, free
// the real buffer, erase the map entry.
//
// PRECONDITIONS (verified by static analysis of all 5 caller blobs)
// ===================================================
// - DpIspStream is non-virtual (no vtable). Method dispatch is direct.
// - Single exported ctor (_ZN11DpIspStreamC1ENS_13ISPStreamTypeE). No
//   versioned entry point. All constructions intercepted.
// - Callers treat DpIspStream as an opaque pointer -- no inline field
//   reads or writes into [0, 0x5a8) (static analysis found 0 hits
//   across all 5 blobs with two independent scans).
//
// THREE METHODS HAVE N-ERA<->A11 SIGNATURE DRIFT
// ==============================================================
// - setSrcConfig (7-arg N-era -> 8-arg A11 with DpSecure)
// - setDstConfig (7-arg N-era -> 8-arg A11 with DpSecure)
// - startStream (void N-era -> timeval* A11)
// These were previously handled by libdpframework_n_era_shim.so. This
// compat shim TAKES OVER their handling for DpIspStream (combining
// shadow-remap + signature-adapt in one step), and must be added to
// each caller blob's DT_NEEDED BEFORE libdpframework_n_era_shim.so so
// the linker resolves the compat shim's export first. n_era_shim still
// handles DpBlitStream::invalidate (unrelated to DpIspStream).
//
// SYMBOLS EXPORTED (17)
// =====================
// Ctor/dtor:
//   _ZN11DpIspStreamC1ENS_13ISPStreamTypeE   ctor
//   _ZN11DpIspStreamD1Ev                      dtor
// Stream lifecycle (3):
//   _ZN11DpIspStream11startStreamEv           (N-era void -- forwards with timeval)
//   _ZN11DpIspStream10stopStreamEv
//   _ZN11DpIspStream15dequeueFrameEndEPj
// Buffer queue (5):
//   _ZN11DpIspStream14queueSrcBufferEPPvPjS2_i
//   _ZN11DpIspStream14queueSrcBufferEPvjj         (3-arg short form)
//   _ZN11DpIspStream14queueDstBufferEiPPvPjS2_i
//   _ZN11DpIspStream16dequeueSrcBufferEv
//   _ZN11DpIspStream16dequeueDstBufferEiPPvb
// Configuration (7):
//   _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb
//     (N-era 9-arg -- forwards with DpSecure=0 to A11 10-arg)
//   _ZN11DpIspStream12setSrcConfigE13DP_COLOR_ENUMiiib   (short 5-arg -- unchanged)
//   _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb
//     (N-era 10-arg -- forwards with DpSecure=0 to A11 11-arg)
//   _ZN11DpIspStream10setSrcCropEiiiiii
//   _ZN11DpIspStream11setRotationEii
//   _ZN11DpIspStream13setFlipStatusEib
//   _ZN11DpIspStream12setParameterER23ISP_TPIPE_CONFIG_STRUCTj
//
// REAL-SIDE SYMBOLS RESOLVED VIA dlsym from libdpframework.so
// ==========================================================
// Same mangled names except:
//   _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream11startStreamEP7timeval

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>

#include <unordered_map>
#include <mutex>

// N-era -> A11 DP_COLOR_ENUM translation.
#include "DpColorXlate.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
// A11 DpIspStream struct reaches at least offset 0x46f8 (confirmed by
// disassembly of A11 libdpframework.so).
// Allocate 0x4800 (18 KB) to give comfortable tail headroom.
static constexpr size_t kA11AllocBytes = 0x4800;

// ---------------------------------------------------------------------------
// Enum / type forward declarations for correct name-mangling.
// These types match the N-era caller blob signatures; DpSecure is the
// A11-only enum parameter appended to setSrcConfig/setDstConfig.
// ---------------------------------------------------------------------------
enum DP_COLOR_ENUM : int { };
enum DP_PROFILE_ENUM : int { };
enum DpInterlaceFormat : int { };
enum DpSecure : int { DP_SECURE_NONE = 0 };
struct DpRect;
struct ISP_TPIPE_CONFIG_STRUCT;

// DpIspStream::ISPStreamType -- mangled as `NS_13ISPStreamTypeE`, i.e.,
// a nested enum inside DpIspStream. We declare the class and its methods
// here so the compiler produces the right mangled exports. Method bodies
// appear out-of-class below.
class DpIspStream {
public:
    enum ISPStreamType : int { };

    // --- ctor/dtor ---
    DpIspStream(ISPStreamType streamType);
    ~DpIspStream();

    // --- stream lifecycle ---
    int startStream();          // N-era void form
    int stopStream();
    int dequeueFrameEnd(unsigned int*);

    // --- buffer queue ---
    int queueSrcBuffer(void**, unsigned int*, unsigned int*, int);
    int queueSrcBuffer(void*, unsigned int, unsigned int);
    int queueDstBuffer(int, void**, unsigned int*, unsigned int*, int);
    int dequeueSrcBuffer();
    int dequeueDstBuffer(int, void**, bool);

    // --- configuration ---
    // Two setSrcConfig forms: the N-era 9-arg (signature-drift from A11)
    // and the short 5-arg (unchanged between N-era and A11).
    int setSrcConfig(int, int, int, int,
                     DP_COLOR_ENUM, DP_PROFILE_ENUM,
                     DpInterlaceFormat, DpRect*, bool);
    int setSrcConfig(DP_COLOR_ENUM, int, int, int, bool);
    // setDstConfig has only one used form -- 10-arg N-era (drifts to 11-arg A11).
    int setDstConfig(int, int, int, int, int,
                     DP_COLOR_ENUM, DP_PROFILE_ENUM,
                     DpInterlaceFormat, DpRect*, bool);
    int setSrcCrop(int, int, int, int, int, int);
    int setRotation(int, int);
    int setFlipStatus(int, bool);
    int setParameter(ISP_TPIPE_CONFIG_STRUCT&, unsigned int);
};

// ---------------------------------------------------------------------------
// Shadow map: caller facade pointer -> real A11 pointer.
//
// LEAK-ON-SHUTDOWN: the mutex and map are heap-allocated
// via pthread_once and intentionally NEVER deleted. This avoids a static-
// init-order bug where the Meyers-singleton std::mutex dtor ran at
// __cxa_finalize BEFORE lingering DpIspStream::~DpIspStream() dtors
// (triggered by HAL respawn cycles) attempted to lock the destroyed
// mutex -> FORTIFY: pthread_mutex_lock on destroyed mutex -> SIGABRT.
// Verified by fatal crash at process shutdown: pthread_mutex_lock on
// destroyed mutex.
//
// The leak is correct for C++ runtime safety: the pointers live for the
// entire process lifetime so any late-running dtor can always lock. No
// memory is ever reclaimed by us; process teardown is the only "free".
// ---------------------------------------------------------------------------
namespace {

static pthread_once_t s_shadow_once = PTHREAD_ONCE_INIT;
static std::mutex* g_shadow_mutex = nullptr;
static std::unordered_map<void*, void*>* g_shadow_map = nullptr;

static void shadow_init() {
    g_shadow_mutex = new std::mutex();
    g_shadow_map = new std::unordered_map<void*, void*>();
}

inline std::mutex& shadow_mutex() {
    pthread_once(&s_shadow_once, shadow_init);
    return *g_shadow_mutex;
}

inline std::unordered_map<void*, void*>& shadow_map() {
    pthread_once(&s_shadow_once, shadow_init);
    return *g_shadow_map;
}

void shadow_insert(void* facade, void* real) {
    std::lock_guard<std::mutex> lk(shadow_mutex());
    shadow_map()[facade] = real;
}

// Lookup the real A11 pointer for a facade pointer. Returns nullptr if
// not found -- that would indicate a caller deref'ing a stale or
// un-ctor'd pointer (shouldn't happen with well-formed code).
void* shadow_lookup(void* facade) {
    std::lock_guard<std::mutex> lk(shadow_mutex());
    auto it = shadow_map().find(facade);
    return it == shadow_map().end() ? nullptr : it->second;
}

void* shadow_erase(void* facade) {
    std::lock_guard<std::mutex> lk(shadow_mutex());
    auto it = shadow_map().find(facade);
    if (it == shadow_map().end()) return nullptr;
    void* real = it->second;
    shadow_map().erase(it);
    return real;
}

// ---------------------------------------------------------------------------
// Lazy dlsym resolver -- cached function pointers for A11 DpIspStream
// symbols. Resolved the first time they're used; thereafter the cache
// entry is a plain function pointer, no lock needed (atomic-init is
// harmless since every thunk will re-resolve deterministically).
// ---------------------------------------------------------------------------

static pthread_once_t s_dlopen_once = PTHREAD_ONCE_INIT;
static void* s_libdpframework_handle = nullptr;

static void dlopen_libdpframework() {
    // RTLD_NOW: resolve all symbols immediately so later dlsym is fast.
    // RTLD_GLOBAL: not needed -- all references are through our cached
    // function pointers, not through the global symbol chain.
    s_libdpframework_handle = dlopen("libdpframework.so", RTLD_NOW);
    // If dlopen fails, dlsym will fall through to return nullptr and
    // every thunk will no-op. We intentionally do not abort() here --
    // the camerahalserver will see a log spam but not crash on shim
    // load. Individual thunks check for resolution failure and return
    // a sensible default (0 or -1 depending on signature).
}

static void* resolve(const char* sym) {
    pthread_once(&s_dlopen_once, dlopen_libdpframework);
    if (!s_libdpframework_handle) return nullptr;
    return dlsym(s_libdpframework_handle, sym);
}

// Helper: build a timeval pointing 30 s in the future. A11 startStream
// (and similar timed methods) dereference the timeval* without null
// check; passing a far-future deadline mirrors what the N-era code
// would have done before the arg was added.
struct timeval make_far_future_timeval() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec += 30;
    return tv;
}

}  // namespace

// ---------------------------------------------------------------------------
// C++ method definitions. The compiler produces the mangled export
// symbols for each. The runtime linker resolves the caller's
// `_ZN11DpIspStream...` references to THESE thunks (because of
// DT_NEEDED ordering placing libdpframework_compat.so first).
//
// Each thunk:
//   1. Looks up the real A11 pointer via shadow_lookup(this).
//   2. Caches the A11 symbol's function pointer via resolve().
//   3. Forwards the call with `real` as the A11 `this`.
// ---------------------------------------------------------------------------

// ---- ctor ----
// `_ZN11DpIspStreamC1ENS_13ISPStreamTypeE`
// C1 (complete) ctor. On ARM AAPCS the `this` pointer is in r0 and the
// streamtype enum is in r1. We must NOT return anything (void).
DpIspStream::DpIspStream(DpIspStream::ISPStreamType streamType) {
    // `this` is the caller's facade pointer (from operator new(0x5a8)
    // or equivalent). We allocate a separate A11-sized real buffer.
    void* facade = this;
    void* real = malloc(kA11AllocBytes);
    if (!real) {
        // Allocation failure -- fail silently; caller will segfault on
        // subsequent method call via shadow_lookup returning nullptr.
        // Not great, but also not the failure mode we're solving for.
        return;
    }
    // Zero-initialize the real buffer so the A11 ctor starts from a
    // clean slate. (A11 ctor also zeros many fields, but this guards
    // against any field the A11 ctor skips.)
    memset(real, 0, kA11AllocBytes);

    // Resolve real ctor: _ZN11DpIspStreamC1ENS_13ISPStreamTypeE
    // ARM C++ ABI: C1 and C2 are typically aliased at the same VA for
    // non-virtual classes with no base-class ctors (C1/C2 share the same
    // VA in A11 libdpframework, confirmed by disassembly). Resolving C1
    // gets us the right entry.
    using Ctor = void (*)(void*, int);
    Ctor real_ctor = reinterpret_cast<Ctor>(
        resolve("_ZN11DpIspStreamC1ENS_13ISPStreamTypeE"));
    if (!real_ctor) {
        // Real ctor unresolvable -- release the A11 buffer and give up.
        free(real);
        return;
    }
    real_ctor(real, static_cast<int>(streamType));

    // Record the facade->real mapping.
    shadow_insert(facade, real);

    // Leave the facade's 0x5a8 bytes unmodified. Per static analysis the
    // callers do not read or write these bytes; they treat the
    // DpIspStream* as opaque. Should a runtime bug reveal some caller-
    // side read, we can revisit by mirroring select N-era-layout fields
    // from `real` into `facade` here. For now, leave the heap block as-
    // is (uninitialized operator new content). If a caller DID read
    // offset 0 it would get garbage, but static analysis found no such read.
}

// ---- dtor ----
// `_ZN11DpIspStreamD1Ev`
DpIspStream::~DpIspStream() {
    void* facade = this;
    void* real = shadow_erase(facade);
    if (!real) {
        // No mapping -- either ctor failed or caller double-deleted.
        // Nothing to do.
        return;
    }
    using Dtor = void (*)(void*);
    Dtor real_dtor = reinterpret_cast<Dtor>(
        resolve("_ZN11DpIspStreamD1Ev"));
    if (real_dtor) {
        real_dtor(real);
    }
    free(real);
}

// ---- method thunks (15 total) ----
// Macros to cut boilerplate. Each macro produces a C++ method that
// delegates to the A11 symbol with the same mangled name (unless
// explicitly overridden, e.g., for setSrcConfig 7-arg -> 8-arg drift).

// Explicit per-method definitions. Each body:
//   1. Looks up the real A11 pointer via shadow_lookup(this).
//   2. Caches the A11 symbol's function pointer via resolve() in a
//      function-local static (thread-safe init since C++11).
//   3. Forwards the call with `real` as the A11 `this`, passing through
//      all arguments unchanged except for setSrcConfig / setDstConfig /
//      startStream where the signature drifted between N-era and A11.

// --- queueSrcBuffer(void**, unsigned int*, unsigned int*, int) ---
// A11 form: identical. Forwards to same mangled symbol.
int DpIspStream::queueSrcBuffer(void** buf, unsigned int* pA, unsigned int* pB, int i) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, void**, unsigned int*, unsigned int*, int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream14queueSrcBufferEPPvPjS2_i"));
    if (!real || !fn) return -1;
    return fn(real, buf, pA, pB, i);
}

// --- queueSrcBuffer(void*, unsigned int, unsigned int) 3-arg short form ---
int DpIspStream::queueSrcBuffer(void* buf, unsigned int a, unsigned int b) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, void*, unsigned int, unsigned int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream14queueSrcBufferEPvjj"));
    if (!real || !fn) return -1;
    return fn(real, buf, a, b);
}

// --- queueDstBuffer(int, void**, unsigned int*, unsigned int*, int) ---
int DpIspStream::queueDstBuffer(int i, void** buf, unsigned int* pA,
                                unsigned int* pB, int flag) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, void**, unsigned int*, unsigned int*, int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream14queueDstBufferEiPPvPjS2_i"));
    if (!real || !fn) return -1;
    return fn(real, i, buf, pA, pB, flag);
}

// --- dequeueSrcBuffer() ---
int DpIspStream::dequeueSrcBuffer() {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream16dequeueSrcBufferEv"));
    if (!real || !fn) return -1;
    return fn(real);
}

// --- dequeueDstBuffer(int, void**, bool) ---
int DpIspStream::dequeueDstBuffer(int i, void** buf, bool b) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, void**, bool);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream16dequeueDstBufferEiPPvb"));
    if (!real || !fn) return -1;
    return fn(real, i, buf, b);
}

// --- dequeueFrameEnd(unsigned int*) ---
int DpIspStream::dequeueFrameEnd(unsigned int* p) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, unsigned int*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream15dequeueFrameEndEPj"));
    if (!real || !fn) return -1;
    return fn(real, p);
}

// --- stopStream() ---
int DpIspStream::stopStream() {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream10stopStreamEv"));
    if (!real || !fn) return -1;
    return fn(real);
}

// --- startStream() -- N-era void form -> A11 startStream(timeval*) ---
// This is a SIGNATURE-DRIFT method. The N-era caller invokes the no-arg
// form; A11 expects a timeval* that it dereferences without null check.
// We pass a stack-allocated "30 s in the future" deadline.
int DpIspStream::startStream() {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, struct timeval*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream11startStreamEP7timeval"));
    if (!real || !fn) return -1;
    struct timeval tv = make_far_future_timeval();
    return fn(real, &tv);
}

// --- setSrcConfig(int,int,int,int,DP_COLOR_ENUM,DP_PROFILE_ENUM,
//                  DpInterlaceFormat,DpRect*,bool) N-era 9-arg form ---
// SIGNATURE-DRIFT method. A11 adds DpSecure. Forward with DP_SECURE_NONE.
int DpIspStream::setSrcConfig(int w, int h, int pitch, int scanline,
                               DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                               DpInterlaceFormat interlace, DpRect* roi,
                               bool flip) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, int, int, int, DP_COLOR_ENUM,
                       DP_PROFILE_ENUM, DpInterlaceFormat, DpRect*, bool,
                       DpSecure);
    static Fn fn = reinterpret_cast<Fn>(resolve(
        "_ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (!real || !fn) return -1;
    // N-era -> A11 color-format translation (see DpColorXlate.h).
    DP_COLOR_ENUM xcolor = static_cast<DP_COLOR_ENUM>(
        dpframework_compat::xlate_color_n_to_a(static_cast<int32_t>(color)));
    return fn(real, w, h, pitch, scanline, xcolor, profile, interlace, roi, flip,
              DP_SECURE_NONE);
}

// --- setSrcConfig(DP_COLOR_ENUM, int, int, int, bool) 5-arg short form ---
// Non-drifted by mangling. Present in both N-era and A11. Semantic args:
// (color, width, height, pitch, flip).
//
// UVPitch post-field patch: A11's DpChannel::setSourcePort
// dropped N-era's "skip UVPitch validation for single-plane color formats"
// branch, so UVPitch=0 now fails the (bpp*width)/8 <= UVPitch check for
// single-plane sensor buffers (e.g., RAW10 packed: w=1280, pitch=1610,
// UVPitch=0 => log spam "DpChannel: invalid width/height/YPitch/UVPitch(0)"
// at ~10 Hz from libimageio_plat_drv::MdpMgrImp::startMdp via the A11 4-arg
// setSrcConfig, which hardcodes 0 at real-object offset 0x46e4).
//
// Fix: after the real A11 method populates the real object, overwrite
// [real+0x46e4] with the caller's pitch argument. The 4-arg form is for
// single-plane formats only (9/10-arg forms carry a separate `scanline`
// for YV12/NV12); treating scanline == pitch for single-plane is correct
// and satisfies A11's validation.
//
// Empirically confirmed: A11 libdpframework.so DpChannel::setSourcePort
// offset 0x46e4 is the UVPitch/scanline slot.
int DpIspStream::setSrcConfig(DP_COLOR_ENUM color, int a, int b, int c, bool f) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, DP_COLOR_ENUM, int, int, int, bool);
    static Fn fn = reinterpret_cast<Fn>(resolve(
        "_ZN11DpIspStream12setSrcConfigE13DP_COLOR_ENUMiiib"));
    if (!real || !fn) return -1;
    // N-era -> A11 color-format translation (see DpColorXlate.h).
    DP_COLOR_ENUM xcolor = static_cast<DP_COLOR_ENUM>(
        dpframework_compat::xlate_color_n_to_a(static_cast<int32_t>(color)));
    int rc = fn(real, xcolor, a, b, c, f);
    // Patch UVPitch/scanline slot to equal pitch for single-plane formats.
    // `c` is the caller's pitch arg (YPitch).
    *reinterpret_cast<int32_t*>(static_cast<char*>(real) + 0x46e4) = c;
    return rc;
}

// --- setDstConfig(int,int,int,int,int,DP_COLOR_ENUM,DP_PROFILE_ENUM,
//                  DpInterlaceFormat,DpRect*,bool) N-era 10-arg ---
// SIGNATURE-DRIFT. A11 adds DpSecure. Forward with DP_SECURE_NONE.
int DpIspStream::setDstConfig(int port, int w, int h, int pitch, int scanline,
                               DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                               DpInterlaceFormat interlace, DpRect* roi,
                               bool flip) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, int, int, int, int, DP_COLOR_ENUM,
                       DP_PROFILE_ENUM, DpInterlaceFormat, DpRect*, bool,
                       DpSecure);
    static Fn fn = reinterpret_cast<Fn>(resolve(
        "_ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (!real || !fn) return -1;
    // N-era -> A11 color-format translation (see DpColorXlate.h).
    DP_COLOR_ENUM xcolor = static_cast<DP_COLOR_ENUM>(
        dpframework_compat::xlate_color_n_to_a(static_cast<int32_t>(color)));
    return fn(real, port, w, h, pitch, scanline, xcolor, profile, interlace,
              roi, flip, DP_SECURE_NONE);
}

// --- setSrcCrop(int,int,int,int,int,int) ---
int DpIspStream::setSrcCrop(int a, int b, int c, int d, int e, int f) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, int, int, int, int, int);
    static Fn fn = reinterpret_cast<Fn>(resolve(
        "_ZN11DpIspStream10setSrcCropEiiiiii"));
    if (!real || !fn) return -1;
    return fn(real, a, b, c, d, e, f);
}

// --- setRotation(int, int) ---
int DpIspStream::setRotation(int a, int b) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream11setRotationEii"));
    if (!real || !fn) return -1;
    return fn(real, a, b);
}

// --- setFlipStatus(int, bool) ---
int DpIspStream::setFlipStatus(int a, bool b) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, int, bool);
    static Fn fn = reinterpret_cast<Fn>(
        resolve("_ZN11DpIspStream13setFlipStatusEib"));
    if (!real || !fn) return -1;
    return fn(real, a, b);
}

// --- setParameter(ISP_TPIPE_CONFIG_STRUCT&, unsigned int) ---
int DpIspStream::setParameter(ISP_TPIPE_CONFIG_STRUCT& cfg, unsigned int n) {
    void* real = shadow_lookup(this);
    using Fn = int (*)(void*, ISP_TPIPE_CONFIG_STRUCT&, unsigned int);
    static Fn fn = reinterpret_cast<Fn>(resolve(
        "_ZN11DpIspStream12setParameterER23ISP_TPIPE_CONFIG_STRUCTj"));
    if (!real || !fn) return -1;
    return fn(real, cfg, n);
}
