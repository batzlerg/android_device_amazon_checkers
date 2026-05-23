// libdpframework_compat -- DpBlitStream hybrid shadow-alloc + facade field sync
//                          + VA->ION memcpy bridge in 2-arg setSrcBuffer.
//
// Implements shadow-alloc + field sync for DpBlitStream and a
// VA->ION memcpy bridge in setSrcBuffer(void*, uint).
//
// PROBLEM HISTORY
// ===============
// An earlier plain-forwarder (dlsym + forward `this`) worked on the assumption
// A11's DpBlitStream struct fits in the caller's stack slot (1.34x drift
// from 0x130 to 0x198 was deemed tail-padding tolerant). After patching
// caller DT_NEEDED so compat wins GOT resolution, preview advanced far enough to hit
// libfeatureio stack corruption: the A11 ctor writes to +0x134..+0x197 in
// the caller's stack frame, clobbering the saved lr/fp. SIGSEGV at dtor.
//
// Caller-side audit found 13 compiler-inlined direct
// field accesses into the DpBlitStream stack slot across all 4
// stack-allocating callers:
//   - offset 0x8c : 4-byte read + conditional 4-byte write (rotation-like field)
//   - offset 0x90 : 1-byte conditional write (dirty flag)
// Pure shadow-alloc (heap-allocate a separate A11-sized real buffer and
// leave the facade slot untouched) silently DROPS these mutations --
// caller reads stale stack bytes, caller writes to dead stack. The
// subsequent invalidate() sees an unchanged real object.
//
// Verified: A11 DpBlitStream ctor writes to 0x8c (4B, init=0)
// and 0x90 (halfword, init=0) -- identical semantics to N-era. The two
// caller-touched fields are preserved in the N-era->A11 transition
// (tail-growth, not reshuffle). This hybrid approach is therefore layout-safe.
//
// SOLUTION -- HYBRID SHADOW-ALLOC + FACADE FIELD SYNC
// =============================================================
// At ctor:
//   1. Allocate a separate heap buffer sized 0x198 bytes (A11 struct).
//   2. Invoke the real A11 ctor on it (writes up to +0x198, stays in-bounds).
//   3. Record facade_ptr -> real_ptr in a shadow map.
//   4. Initialize the facade slot's 0x8c/0x90 bytes to match the A11 real
//      object's initial values (0/0) so caller-inlined reads see valid state.
// At each method thunk entry:
//   a. Look up real_ptr via facade_ptr.
//   b. Copy facade[0x8c] (4B) -> real[0x8c], facade[0x90] (1B) -> real[0x90].
//      This pushes any caller-inlined writes into the A11 real object before
//      the A11 method reads them.
//   c. Call the A11 method on real.
//   d. Copy real[0x8c] -> facade[0x8c], real[0x90] -> facade[0x90]. Pulls
//      any A11-side mutations back into the facade so subsequent caller
//      inline reads see current values.
// At dtor: reverse -- invoke real dtor, free heap buffer, erase map entry.
//
// FIELD SYNC SCOPE
// ================
// Exactly 5 bytes are synced each way: uint32 at 0x8c, uint8 at 0x90.
// Static analysis found NO other in-slot caller writes (one post-dtor
// cleanup byte at slot+0 in camadapter -- RAII liveness flag -- is not a
// DpBlitStream field and should not be synced). The byte at 0x91 (high
// byte of the halfword the ctor initializes) is never mutated by callers
// and retains the A11 initial value. Sync widths deliberately match
// caller write widths -- a halfword sync at 0x90 would zero 0x91 on every
// call, which is likely inert but unnecessary.
//
// PRECONDITIONS (verified)
// ========================
// - DpBlitStream is non-virtual (no vtable). Method dispatch is direct.
// - Single 0-arg ctor (_ZN12DpBlitStreamC1Ev). All constructions intercepted.
// - A11 ctor writes to slot+0x8c (4B, init 0) and slot+0x90 (halfword, init 0).
// - N-era ctor writes identical 4B/halfword at identical offsets, identical
//   init values. Field layout preserved across drift.
//
// SYMBOLS PROVIDED (13 class-method exports, identical mangled names)
// =================================================================================
//   _ZN12DpBlitStreamC1Ev          (ctor)
//   _ZN12DpBlitStreamC2Ev          (ctor alias -- Itanium C1/C2)
//   _ZN12DpBlitStreamD1Ev          (dtor)
//   _ZN12DpBlitStreamD2Ev          (dtor alias)
//   _ZN12DpBlitStream10invalidateEv                     (N-era 0-arg, drifted; adapts to A11 timeval*)
//   _ZN12DpBlitStream12setSrcConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect
//   _ZN12DpBlitStream12setDstConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect
//   _ZN12DpBlitStream12setSrcBufferEiPjj
//   _ZN12DpBlitStream12setSrcBufferEPvj
//   _ZN12DpBlitStream12setSrcBufferEPPvPjj
//   _ZN12DpBlitStream12setDstBufferEiPjj
//   _ZN12DpBlitStream12setDstBufferEPvj
//   _ZN12DpBlitStream12setDstBufferEPPvPjj
//
// Free-function + A11-form re-exports:
//   _Z45tpipe_main_query_platform_working_buffer_sizei
//   _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream11startStreamEP7timeval
//   _ZN12DpBlitStream10invalidateEP7timeval
//
// COEXISTENCE WITH libdpframework_n_era_shim.so
// ===================================================================
// n_era_shim also exports DpBlitStream10invalidateEv. With compat ordered
// first in DT_NEEDED, compat wins. Both semantics are identical (30s
// far-future timeval -> A11 invalidate(timeval*)).
//
// THREAD SAFETY
// =============
// Shadow map access is serialized via a mutex. Map access is scoped to
// ctor/dtor/method-entry lookup only; the mutex is NOT held across the
// real-method call. Stack-allocated DpBlitStream is per-thread anyway
// (the 4 stack-allocating callers are all within per-frame scoped
// functions: imageTransform, convertImage, doRGB565Buffer_DDP,
// doYV12Buffer_DDP). Contention on the shadow-map mutex should be low.

// VA->ION memcpy bridge
// ==========================================================
// PROBLEM: A11 libdpframework's DpMemory::Factory dispatches VA inputs to a
// new VA-mode DpIonHandler ctor (`DpIonHandlerC1(void*, uint)`) that sets a
// discriminator byte at offset +6 = 1. A11 DpIonHandler::mapHWAddress checks
// that byte at entry and emits "not support alloc by va" + returns failure.
// N-era routed VA inputs through DpMmuHandler instead. Callers of VA forms
// in the A11/Lineage camera stack:
//   - libfeatureio.so:  2-arg VA src+dst AND multi-plane VA src+dst
//   - libcam.camadapter.so: 2-arg VA src+dst AND multi-plane VA src+dst
//   - libcam.camshot.so: multi-plane VA dst only
// libfeatureio's doRGB565Buffer_DDP / doYV12Buffer_DDP dispatches via a
// 3-way platform-flag switch between the 2-arg and multi-plane forms.
// The 2-arg bridge alone did not reduce the VA rejection rate; this
// multi-plane form covers the hot path in libfeatureio.
//
// BRIDGED FORMS:
//   setSrcBufferEPvj       (void*, uint)
//   setSrcBufferEPPvPjj    (void**, uint*, uint)
//   setDstBufferEPvj       (void*, uint)
//   setDstBufferEPPvPjj    (void**, uint*, uint)
// (Dual-VA EPPvS1_Pjj forms exist but no caller blob imports them.)
//
// FIX -- SRC bridges:
// In each compat VA forwarder, allocate an ION buffer of total-size bytes
// (sum of plane sizes for multi-plane), mmap it, memcpy each plane's VA
// data in at its offset, ion_sync_fd (cache flush for HW read), munmap,
// then forward to A11's FD-form setSrcBuffer(int fd, uint* sizeList,
// uint planeCount) -- registerBufferFD at 0x13f3c writes the SAME fd to
// one slot per plane, so a single fd representing all planes IS the
// expected convention.
//
// FIX -- DST bridges:
// Dst buffers are WRITE targets (MDP writes output). Bridging requires
// readback: allocate ION (do NOT memcpy VA in, it's garbage from caller's
// perspective), forward to FD form. The HW writes into the ION buffer.
// The caller expects to read results from its own VA after invalidate()
// blocks for HW-done. We therefore:
//   - On setDstBuffer(VA): alloc ION, save {va, size, fd} in shadow state
//     as pending_dst_readback. Forward FD form.
//   - On invalidate(): after the real A11 invalidate() returns (HW is
//     guaranteed done), mmap the pending ION fd, memcpy ION -> caller's
//     VA, munmap. Readback semantically matches what callers expect
//     from an N-era synchronous blit.
//   - On dtor: if pending readback remains (no invalidate was called
//     between setDst and dtor -- defensive), skip; close fd.
//
// OWNERSHIP: Each DpBlitStream facade holds at most one outstanding
// compat-owned src ION fd and one dst ION fd at a time. On the next
// setSrcBuffer(VA)/setDstBuffer(VA) call we close the previous fd before
// allocating a new one. At dtor, we close any outstanding fds.
//
// LIBION: /system/lib/libion.so is present on checkers (LineageOS 18.1) and
// already loaded into camerahalserver (DT_NEEDED chain via libdpframework.so
// -> libion_mtk.so -> libion.so). We dlopen it at runtime like we do for
// libdpframework.so; no new DT_NEEDED on our compat shim.
//
// CACHE COHERENCY: Critical. MTK MDP reads/writes ION via bus-master DMA.
// ion_sync_fd() after src memcpy flushes CPU cache so HW observes the
// bytes. For dst readback we rely on the mmap re-map to obtain coherent
// view (system heap is CPU-snoopable; MTK multimedia heap may require an
// explicit invalidate -- empirically the plain ion_sync_fd call wired for
// HW->CPU direction is a no-op for SYSTEM heap but safe to call).
//
// SHUTDOWN ORDERING: Meyers-singleton std::mutex/unordered_map patterns
// have a static-init-order bug where __cxa_finalize may destroy the mutex
// before lingering DpBlitStream/DpIspStream dtors lock it, producing
// FORTIFY: "pthread_mutex_lock on destroyed mutex". Fixed here and in
// DpIspStreamCompat.cpp by initializing the shadow mutex + map via
// pthread_once and NEVER deleting them. The intentional process-lifetime
// leak is correct for C++ runtime safety.
//
// ERROR PATHS (fail-closed, HAL must survive):
//   - libion dlopen fails / symbols missing -> ALOGE once, fall back to
//     forwarding the VA form unchanged. Will still fail at A11 with "not
//     support alloc by va" but no new crash class.
//   - /dev/ion open fails -> ALOGE, same fallback.
//   - ion_alloc_fd fails -> ALOGE, return A11 error (VA-form fallback).
//   - mmap fails -> ALOGE, ion_close(fd), return VA-form fallback.
//   - memcpy cannot fail; if va is invalid the caller is broken -- not our
//     job to guard.
//   - Any multi-plane plane with size==0 or va==NULL: skip that plane's
//     memcpy (defensive); size accounting still uses pSizeList[i].

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <unordered_map>
#include <mutex>

// N-era -> A11 DP_COLOR_ENUM translation.
#include "DpColorXlate.h"

// Minimal ALOGE replacement -- compat shim is -fno-rtti/-fno-exceptions and
// does NOT link liblog (no DT_NEEDED on liblog). Use __android_log_print via
// dlopen on first failure. Rate-limited to one message per call site per
// process lifetime to avoid log spam on repeated failures.
#include <stdio.h>
// Leak-on-shutdown pattern for the log-dedup mutex/map -- avoids the same
// pthread_mutex_lock-on-destroyed-mutex race that afflicted the shadow maps.
// This function may be called during static dtor ordering; be safe.
static pthread_once_t s_loge_once = PTHREAD_ONCE_INIT;
static std::mutex* s_loge_mutex = nullptr;
static std::unordered_map<const char*, bool>* s_loge_seen = nullptr;
static void compat_loge_init() {
    s_loge_mutex = new std::mutex();
    s_loge_seen = new std::unordered_map<const char*, bool>();
}
static void compat_loge_once(const char* tag, const char* msg) {
    pthread_once(&s_loge_once, compat_loge_init);
    if (!s_loge_mutex || !s_loge_seen) {
        fprintf(stderr, "[libdpframework_compat/%s] %s\n", tag, msg);
        return;
    }
    std::lock_guard<std::mutex> lk(*s_loge_mutex);
    if ((*s_loge_seen)[tag]) return;
    (*s_loge_seen)[tag] = true;
    // Fall back to stderr -- liblog optional dlopen is overkill for a one-shot
    // ALOGE; stderr is captured into logcat via init redirection for HAL
    // processes on LineageOS.
    fprintf(stderr, "[libdpframework_compat/%s] %s\n", tag, msg);
}

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
// A11 DpBlitStream ctor largest this-relative store = 0x198. Allocate the
// exact 0x198 bytes -- no tail headroom needed, the ctor stays in-bounds.
// We add a small safety margin (0x20) in case a method later writes past
// the ctor-observed tail; cheap insurance, 0x1b8 bytes still comfortable.
static constexpr size_t kA11BlitStreamAllocBytes = 0x1b8;

// Caller-inlined field offsets (static analysis; A11 layout-matched).
static constexpr size_t kFacadeField0x8cOffset = 0x8c;   // 4-byte
static constexpr size_t kFacadeField0x90Offset = 0x90;   // 1-byte (caller writes strb; halfword in ctor but we sync 1B)

// ---------------------------------------------------------------------------
// Enum / type forward declarations (for correct Itanium mangling).
// Declared at global namespace scope to match the caller's expected
// symbol mangling (e.g., 13DP_COLOR_ENUM not 13dpbs_local_DP_...).
// DpIspStreamCompat.cpp declares the same names at global scope -- the
// C++ one-definition rule allows identical type declarations in multiple
// TUs, and since neither TU defines storage for these (enums are purely
// type-level; DpRect is only used as an opaque pointer), there's no
// cross-TU conflict.
// ---------------------------------------------------------------------------
enum DP_COLOR_ENUM : int { };
enum DpInterlaceFormat : int { };
struct DpRect;

// DpBlitStream class declaration. Non-virtual, caller allocates as opaque
// `this`. We mirror the caller view of the class -- the real A11 layout
// differs in size (0x198 vs caller-assumed 0x130) but the 0x8c/0x90
// fields are at the same offsets in A11 (verified by ctor disassembly).
class DpBlitStream {
public:
    DpBlitStream();
    ~DpBlitStream();

    // signature-drifted 0-arg form: forwards to A11 invalidate(timeval*)
    int invalidate();

    // non-drifted forms (same mangling on N-era and A11)
    int setSrcConfig(int w, int h, DP_COLOR_ENUM c, DpInterlaceFormat il, DpRect* roi);
    int setDstConfig(int w, int h, DP_COLOR_ENUM c, DpInterlaceFormat il, DpRect* roi);
    int setSrcBuffer(int fd, unsigned int* pSizeList, unsigned int planeCount);
    int setSrcBuffer(void* va, unsigned int size);
    int setSrcBuffer(void** pVaList, unsigned int* pSizeList, unsigned int planeCount);
    int setDstBuffer(int fd, unsigned int* pSizeList, unsigned int planeCount);
    int setDstBuffer(void* va, unsigned int size);
    int setDstBuffer(void** pVaList, unsigned int* pSizeList, unsigned int planeCount);
};

// ---------------------------------------------------------------------------
// Shadow map: facade pointer (caller's stack slot addr) -> BlitShadow (real
// A11 heap ptr + src/dst ION state for VA->ION bridges + dst readback queue).
//
// LEAK-ON-SHUTDOWN: the mutex and map are heap-allocated via
// pthread_once and never deleted. Their lifetime spans the whole process so
// late-running DpBlitStream dtors at shutdown can safely lock. See
// fatal crash at process shutdown was the original bug this avoids.
// ---------------------------------------------------------------------------
namespace {

// Per-plane pending-dst-readback descriptor. Up to 3 planes (MTK YV12/NV12).
struct DstReadback {
    void*  va;           // caller's VA for this plane (nullptr = unused)
    size_t size;         // bytes to copy back
    size_t offset;       // offset into the single ION buffer where this plane lives
};

struct BlitShadow {
    void* real;              // A11 heap buffer (0x198 ctor writes)

    // compat-owned ION state (src and dst ION fds for VA->ION bridge).
    int   last_src_ion_fd;   // most recent src ION fd, or -1
    int   last_dst_ion_fd;   // most recent dst ION fd (pending readback), or -1
    size_t dst_ion_size;     // total bytes of the dst ION buffer (sum of planes)
    int    dst_plane_count;  // 1..3
    DstReadback dst_planes[3];
    bool   dst_readback_pending;
};

pthread_once_t s_blit_shadow_once = PTHREAD_ONCE_INIT;
std::mutex* g_blit_shadow_mutex = nullptr;
std::unordered_map<void*, BlitShadow>* g_blit_shadow_map = nullptr;

void blit_shadow_init() {
    g_blit_shadow_mutex = new std::mutex();
    g_blit_shadow_map = new std::unordered_map<void*, BlitShadow>();
}

inline std::mutex& blit_shadow_mutex() {
    pthread_once(&s_blit_shadow_once, blit_shadow_init);
    return *g_blit_shadow_mutex;
}

inline std::unordered_map<void*, BlitShadow>& blit_shadow_map() {
    pthread_once(&s_blit_shadow_once, blit_shadow_init);
    return *g_blit_shadow_map;
}

inline BlitShadow make_empty_shadow(void* real) {
    BlitShadow s;
    s.real = real;
    s.last_src_ion_fd = -1;
    s.last_dst_ion_fd = -1;
    s.dst_ion_size = 0;
    s.dst_plane_count = 0;
    for (int i = 0; i < 3; ++i) {
        s.dst_planes[i].va = nullptr;
        s.dst_planes[i].size = 0;
        s.dst_planes[i].offset = 0;
    }
    s.dst_readback_pending = false;
    return s;
}

void blit_shadow_insert(void* facade, void* real) {
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    (*g_blit_shadow_map)[facade] = make_empty_shadow(real);
}

// Lookup; returns nullptr if not found (caller deref of stale/un-ctor'd pointer
// -- defensive, shouldn't happen with well-formed code).
void* blit_shadow_lookup(void* facade) {
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    auto it = g_blit_shadow_map->find(facade);
    return it == g_blit_shadow_map->end() ? nullptr : it->second.real;
}

// Swap in a new compat-owned src ION fd; returns the previous fd (or -1).
int blit_shadow_swap_src_ion_fd(void* facade, int new_fd) {
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    auto it = g_blit_shadow_map->find(facade);
    if (it == g_blit_shadow_map->end()) return -1;
    int prev = it->second.last_src_ion_fd;
    it->second.last_src_ion_fd = new_fd;
    return prev;
}

// Install a new dst readback descriptor. Returns the previous dst fd (if any)
// so the caller can close it OUTSIDE the shadow-map mutex. NOTE: if the
// previous dst had a pending readback that wasn't serviced (no invalidate()
// between setDst and next setDst), the pending readback is DROPPED. This is
// the same semantics as N-era where calling setDst twice in a row without
// invalidating discards the first blit.
int blit_shadow_install_dst(void* facade, int new_fd, size_t total_size,
                            int plane_count, const DstReadback planes[3]) {
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    auto it = g_blit_shadow_map->find(facade);
    if (it == g_blit_shadow_map->end()) return -1;
    int prev = it->second.last_dst_ion_fd;
    it->second.last_dst_ion_fd = new_fd;
    it->second.dst_ion_size = total_size;
    it->second.dst_plane_count = plane_count;
    for (int i = 0; i < 3; ++i) it->second.dst_planes[i] = planes[i];
    it->second.dst_readback_pending = (new_fd >= 0);
    return prev;
}

// Snapshot the pending dst readback for a facade and clear the pending flag
// atomically. The caller performs the actual mmap/memcpy/munmap outside the
// mutex (no syscalls under the shadow-map lock). The returned fd is NOT
// closed -- it remains in the shadow state so the next setDst can close it
// (or the dtor will).
struct DstReadbackSnapshot {
    bool   valid;
    int    fd;
    size_t total_size;
    int    plane_count;
    DstReadback planes[3];
};
DstReadbackSnapshot blit_shadow_take_dst_readback(void* facade) {
    DstReadbackSnapshot snap;
    snap.valid = false;
    snap.fd = -1;
    snap.total_size = 0;
    snap.plane_count = 0;
    for (int i = 0; i < 3; ++i) {
        snap.planes[i].va = nullptr;
        snap.planes[i].size = 0;
        snap.planes[i].offset = 0;
    }
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    auto it = g_blit_shadow_map->find(facade);
    if (it == g_blit_shadow_map->end()) return snap;
    if (!it->second.dst_readback_pending) return snap;
    if (it->second.last_dst_ion_fd < 0) return snap;
    snap.valid = true;
    snap.fd = it->second.last_dst_ion_fd;
    snap.total_size = it->second.dst_ion_size;
    snap.plane_count = it->second.dst_plane_count;
    for (int i = 0; i < 3; ++i) snap.planes[i] = it->second.dst_planes[i];
    it->second.dst_readback_pending = false;  // consumed
    return snap;
}

// Return the final (real, src_fd, dst_fd, pending_readback) state on dtor,
// erasing the entry.
struct BlitShadowFinal {
    void* real;
    int   src_ion_fd;
    int   dst_ion_fd;
    bool  dst_readback_pending;
    size_t dst_total_size;
    int    dst_plane_count;
    DstReadback dst_planes[3];
};
BlitShadowFinal blit_shadow_erase(void* facade) {
    BlitShadowFinal out;
    out.real = nullptr;
    out.src_ion_fd = -1;
    out.dst_ion_fd = -1;
    out.dst_readback_pending = false;
    out.dst_total_size = 0;
    out.dst_plane_count = 0;
    for (int i = 0; i < 3; ++i) {
        out.dst_planes[i].va = nullptr;
        out.dst_planes[i].size = 0;
        out.dst_planes[i].offset = 0;
    }
    std::lock_guard<std::mutex> lk(blit_shadow_mutex());
    auto it = g_blit_shadow_map->find(facade);
    if (it == g_blit_shadow_map->end()) return out;
    out.real = it->second.real;
    out.src_ion_fd = it->second.last_src_ion_fd;
    out.dst_ion_fd = it->second.last_dst_ion_fd;
    out.dst_readback_pending = it->second.dst_readback_pending;
    out.dst_total_size = it->second.dst_ion_size;
    out.dst_plane_count = it->second.dst_plane_count;
    for (int i = 0; i < 3; ++i) out.dst_planes[i] = it->second.dst_planes[i];
    g_blit_shadow_map->erase(it);
    return out;
}

// ---------------------------------------------------------------------------
// dlopen-cached handle for real libdpframework.so. Shared in spirit with
// DpIspStreamCompat.cpp but each TU has its own pthread_once to avoid
// cross-TU linkage.
// ---------------------------------------------------------------------------
pthread_once_t s_dlopen_once = PTHREAD_ONCE_INIT;
void* s_libdpframework_handle = nullptr;

void dlopen_libdpframework() {
    s_libdpframework_handle = dlopen("libdpframework.so", RTLD_NOW);
}

void* resolve_blit(const char* sym) {
    pthread_once(&s_dlopen_once, dlopen_libdpframework);
    if (!s_libdpframework_handle) return nullptr;
    return dlsym(s_libdpframework_handle, sym);
}

// ---------------------------------------------------------------------------
// libion runtime dlopen + ION helpers (VA->FD bridge).
//
// libion.so (/system/lib/libion.so) is already loaded into camerahalserver
// via libdpframework.so's DT_NEEDED -> libion_mtk.so -> libion.so. We dlopen
// it here with RTLD_NOW so our compat shim doesn't grow a DT_NEEDED.
//
// AOSP libion wraps raw ion ioctls for us:
//   int ion_open(void)                         -> /dev/ion fd
//   int ion_alloc_fd(int ion_fd, size_t len, size_t align,
//                    uint heap_mask, uint flags, int* out_buf_fd)
//   int ion_sync_fd(int ion_fd, int buf_fd)    -> cache flush (CPU->HW)
//   int ion_close(int ion_fd)
//
// Heap mask: ION_HEAP_SYSTEM (bit 0) is the safe cacheable default on MTK
// legacy ion. We use the heap mask (1<<0) | MTK multimedia bit (1<<10) as a
// simple superset the MDP allocator accepts for blit source buffers. If the
// allocator rejects the mask we ALOGE and fall back to the VA form.
// ---------------------------------------------------------------------------

typedef int (*ion_open_fn)(void);
typedef int (*ion_alloc_fd_fn)(int, size_t, size_t, unsigned int, unsigned int, int*);
typedef int (*ion_sync_fd_fn)(int, int);
typedef int (*ion_close_fn)(int);

struct IonApi {
    void* handle;
    ion_open_fn      open;
    ion_alloc_fd_fn  alloc_fd;
    ion_sync_fd_fn   sync_fd;
    ion_close_fn     close;
    int              devfd;   // cached /dev/ion fd (from ion_open)
    bool             usable;
};

pthread_once_t s_ion_once = PTHREAD_ONCE_INIT;
IonApi s_ion{ nullptr, nullptr, nullptr, nullptr, nullptr, -1, false };

void ion_api_init_once() {
    s_ion.handle = dlopen("libion.so", RTLD_NOW);
    if (!s_ion.handle) {
        compat_loge_once("ion-init", "dlopen(libion.so) failed");
        return;
    }
    s_ion.open     = reinterpret_cast<ion_open_fn>    (dlsym(s_ion.handle, "ion_open"));
    s_ion.alloc_fd = reinterpret_cast<ion_alloc_fd_fn>(dlsym(s_ion.handle, "ion_alloc_fd"));
    s_ion.sync_fd  = reinterpret_cast<ion_sync_fd_fn> (dlsym(s_ion.handle, "ion_sync_fd"));
    s_ion.close    = reinterpret_cast<ion_close_fn>   (dlsym(s_ion.handle, "ion_close"));
    if (!s_ion.open || !s_ion.alloc_fd || !s_ion.sync_fd || !s_ion.close) {
        compat_loge_once("ion-init", "required ion_* symbols missing");
        return;
    }
    int fd = s_ion.open();
    if (fd < 0) {
        compat_loge_once("ion-init", "ion_open(/dev/ion) failed");
        return;
    }
    s_ion.devfd = fd;
    s_ion.usable = true;
}

bool ion_api_ready() {
    pthread_once(&s_ion_once, ion_api_init_once);
    return s_ion.usable;
}

// Copy `size` bytes from `va` into a fresh ION buffer. Returns the dma-buf fd
// (>=0) on success; -1 on any failure. Caller owns the fd.
//
// Heap mask notes: MTK legacy ion (kernel 3.18) accepts raw heap ids via
// (1<<heap_id) in heap_mask. Heap id 0 is the system (scatter-gather, cached)
// heap, which is sufficient for a CPU-memcpy source buffer that the MDP reads
// via a bus-master DMA bus (with ion_sync_fd flushing CPU caches so HW sees
// the data). MTK-specific multimedia heaps are 1<<10..1<<13; we try those as
// a fallback if the system heap is rejected.
int ion_bridge_alloc_and_copy(const void* va, size_t size) {
    if (!ion_api_ready()) return -1;

    // Try heap masks in order: multimedia-contig (1<<10), then system (1<<0).
    // libdpframework's native fd-path uses ION_HEAP_MULTIMEDIA on MTK; match
    // that first so the MDP's ION import path doesn't hit a heap mismatch.
    const unsigned int heap_masks[] = { (1u << 10), (1u << 0) };
    int buf_fd = -1;
    int last_err = 0;
    for (unsigned int mask : heap_masks) {
        int r = s_ion.alloc_fd(s_ion.devfd, size, 0, mask, 0, &buf_fd);
        if (r == 0 && buf_fd >= 0) { last_err = 0; break; }
        last_err = r;
        buf_fd = -1;
    }
    if (buf_fd < 0) {
        compat_loge_once("ion-alloc", "ion_alloc_fd failed on all heap masks");
        return -1;
    }

    void* mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
    if (mapped == MAP_FAILED) {
        compat_loge_once("ion-mmap", "mmap of ION buffer failed");
        close(buf_fd);
        return -1;
    }

    memcpy(mapped, va, size);

    // Flush CPU cache so the MDP (bus master) observes the memcpy'd bytes.
    if (s_ion.sync_fd(s_ion.devfd, buf_fd) != 0) {
        compat_loge_once("ion-sync", "ion_sync_fd failed (continuing)");
        // Non-fatal -- some MTK configs return ENOTTY for SYNC on already-
        // coherent heaps. Continue; the HW may still see the data if the
        // heap is noncached or the HW snoops.
    }

    munmap(mapped, size);
    return buf_fd;
}

// Allocate an ION dma-buf of the given size without copying anything in.
// Used for DST buffers (HW will write into it). Returns -1 on failure.
int ion_bridge_alloc_only(size_t size) {
    if (!ion_api_ready()) return -1;
    const unsigned int heap_masks[] = { (1u << 10), (1u << 0) };
    int buf_fd = -1;
    for (unsigned int mask : heap_masks) {
        int r = s_ion.alloc_fd(s_ion.devfd, size, 0, mask, 0, &buf_fd);
        if (r == 0 && buf_fd >= 0) return buf_fd;
        buf_fd = -1;
    }
    compat_loge_once("ion-alloc-dst", "ion_alloc_fd (dst) failed on all heap masks");
    return -1;
}

// Multi-plane src bridge. Allocates one ION buffer sized sum(sizes[0..planes-1]),
// mmaps it, memcpies each pVaList[i] to its offset, ion_sync_fd, munmaps.
// Returns the fd (caller owns) + writes plane offsets via outOffsets[] if !null.
// Returns -1 on any failure.
int ion_bridge_alloc_and_copy_multi(void** pVaList, const unsigned int* pSizeList,
                                    unsigned int planeCount,
                                    size_t* outTotal, size_t outOffsets[3]) {
    if (!ion_api_ready() || planeCount == 0 || planeCount > 3) return -1;
    size_t total = 0;
    size_t offsets[3] = {0, 0, 0};
    for (unsigned int i = 0; i < planeCount; ++i) {
        offsets[i] = total;
        total += pSizeList ? pSizeList[i] : 0;
    }
    if (total == 0) return -1;

    const unsigned int heap_masks[] = { (1u << 10), (1u << 0) };
    int buf_fd = -1;
    for (unsigned int mask : heap_masks) {
        int r = s_ion.alloc_fd(s_ion.devfd, total, 0, mask, 0, &buf_fd);
        if (r == 0 && buf_fd >= 0) break;
        buf_fd = -1;
    }
    if (buf_fd < 0) {
        compat_loge_once("ion-alloc-multi", "ion_alloc_fd multi failed");
        return -1;
    }

    void* mapped = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
    if (mapped == MAP_FAILED) {
        compat_loge_once("ion-mmap-multi", "mmap of ION multi buffer failed");
        close(buf_fd);
        return -1;
    }

    for (unsigned int i = 0; i < planeCount; ++i) {
        if (pVaList && pVaList[i] && pSizeList && pSizeList[i] > 0) {
            memcpy(static_cast<uint8_t*>(mapped) + offsets[i], pVaList[i], pSizeList[i]);
        }
    }

    if (s_ion.sync_fd(s_ion.devfd, buf_fd) != 0) {
        compat_loge_once("ion-sync-multi", "ion_sync_fd multi failed (continuing)");
    }

    munmap(mapped, total);
    if (outTotal) *outTotal = total;
    if (outOffsets) {
        for (int i = 0; i < 3; ++i) outOffsets[i] = offsets[i];
    }
    return buf_fd;
}

// Close an ION dma-buf fd obtained from any ion_bridge_* helper. Safe on -1.
void ion_bridge_close(int fd) {
    if (fd < 0) return;
    close(fd);
}

// Readback: mmap the ION dst fd, copy each plane back to caller's VA, munmap.
// Safe to call with !pending / fd<0 (no-op). Does NOT close the fd; the caller
// manages fd lifetime via the shadow state.
void ion_bridge_readback_dst(int fd, size_t total_size, int plane_count,
                             const DstReadback planes[3]) {
    if (fd < 0 || total_size == 0 || plane_count <= 0) return;
    void* mapped = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        compat_loge_once("ion-readback-mmap", "mmap for dst readback failed");
        return;
    }
    // We do not explicitly DMA_BUF_SYNC(END) here -- system heap mappings on
    // MTK legacy ion are CPU-coherent (snoopable); multimedia heap may be
    // noncached and HW-written bytes are visible directly. A defensive
    // ion_sync_fd call would need the HW->CPU direction which libion's
    // thin wrapper doesn't expose. If readback shows stale bytes we can
    // escalate to a raw DMA_BUF_IOCTL_SYNC(END|READ) ioctl.
    for (int i = 0; i < plane_count && i < 3; ++i) {
        const DstReadback& p = planes[i];
        if (p.va && p.size > 0 && p.offset + p.size <= total_size) {
            memcpy(p.va, static_cast<const uint8_t*>(mapped) + p.offset, p.size);
        }
    }
    munmap(mapped, total_size);
}

struct timeval make_far_future_timeval_blit() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec += 30;
    return tv;
}

// ---------------------------------------------------------------------------
// Facade <-> real field sync helpers.
//
// Width matches caller-observed write widths (verified by static analysis):
//   0x8c  : uint32 (ldr / strne)
//   0x90  : uint8  (strbne) -- NOT halfword, even though ctor writes halfword.
//           0x91 retains A11 initial value.
// ---------------------------------------------------------------------------
inline void sync_facade_to_real(void* facade, void* real) {
    uint8_t* f = static_cast<uint8_t*>(facade);
    uint8_t* r = static_cast<uint8_t*>(real);
    *reinterpret_cast<uint32_t*>(r + kFacadeField0x8cOffset) =
        *reinterpret_cast<uint32_t*>(f + kFacadeField0x8cOffset);
    *(r + kFacadeField0x90Offset) = *(f + kFacadeField0x90Offset);
}

inline void sync_real_to_facade(void* facade, void* real) {
    uint8_t* f = static_cast<uint8_t*>(facade);
    uint8_t* r = static_cast<uint8_t*>(real);
    *reinterpret_cast<uint32_t*>(f + kFacadeField0x8cOffset) =
        *reinterpret_cast<uint32_t*>(r + kFacadeField0x8cOffset);
    *(f + kFacadeField0x90Offset) = *(r + kFacadeField0x90Offset);
}

}  // namespace

// ---------------------------------------------------------------------------
// ctor -- hybrid shadow-alloc + initial-field mirror.
// ---------------------------------------------------------------------------
DpBlitStream::DpBlitStream() {
    void* facade = this;

    void* real = malloc(kA11BlitStreamAllocBytes);
    if (!real) {
        // Allocation failure -- leave facade uninitialized. Subsequent method
        // thunks will find no shadow mapping and no-op with -1.
        return;
    }
    memset(real, 0, kA11BlitStreamAllocBytes);

    using Ctor = void (*)(void*);
    Ctor real_ctor = reinterpret_cast<Ctor>(
        resolve_blit("_ZN12DpBlitStreamC1Ev"));
    if (!real_ctor) {
        free(real);
        return;
    }
    real_ctor(real);

    blit_shadow_insert(facade, real);

    // Mirror the two caller-visible fields from the A11 real object into
    // the facade slot. A11 ctor initialized both to zero -- matches N-era.
    // Caller's first inline `ldr [slot+0x8c]` will read the A11 initial
    // value (0); if the caller's new value equals 0 the write-side is
    // skipped; if different the caller writes into the facade slot, and
    // the next method thunk's sync_facade_to_real pushes the new value
    // into real.
    sync_real_to_facade(facade, real);
}

// ---------------------------------------------------------------------------
// dtor
// ---------------------------------------------------------------------------
DpBlitStream::~DpBlitStream() {
    void* facade = this;
    BlitShadowFinal final = blit_shadow_erase(facade);
    if (!final.real) {
        return;
    }
    // Release any outstanding compat-owned ION fds. Do this BEFORE
    // invoking the real dtor so the A11 object doesn't reach into a stale fd
    // during teardown. Any pending dst readback that was not flushed by
    // invalidate() is discarded (defensive; indicates caller bug).
    ion_bridge_close(final.src_ion_fd);
    ion_bridge_close(final.dst_ion_fd);

    using Dtor = void (*)(void*);
    Dtor real_dtor = reinterpret_cast<Dtor>(
        resolve_blit("_ZN12DpBlitStreamD1Ev"));
    if (real_dtor) {
        real_dtor(final.real);
    }
    free(final.real);
}

// ---------------------------------------------------------------------------
// Method thunk pattern:
//   1. Look up real via shadow map.
//   2. sync_facade_to_real  (push caller-inlined writes into real).
//   3. Call real method.
//   4. sync_real_to_facade  (pull any A11 mutations back into facade).
//   5. Return.
// ---------------------------------------------------------------------------

// invalidate() -- SIGNATURE DRIFT (0-arg N-era -> timeval* A11)
// After the A11 invalidate() blocks-until-HW-done, perform any pending
// dst readback (copy ION contents back to caller's VA buffers).
int DpBlitStream::invalidate() {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;
    using Fn = int (*)(void*, struct timeval*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_ZN12DpBlitStream10invalidateEP7timeval"));
    if (!fn) return -1;
    sync_facade_to_real(facade, real);
    struct timeval tv = make_far_future_timeval_blit();
    int rv = fn(real, &tv);
    sync_real_to_facade(facade, real);

    // Dst readback: caller expects to read back HW-written data from its VA
    // after invalidate() returns. Snapshot + clear the pending flag under
    // the shadow-map mutex; do the mmap/memcpy outside the lock.
    DstReadbackSnapshot snap = blit_shadow_take_dst_readback(facade);
    if (snap.valid) {
        ion_bridge_readback_dst(snap.fd, snap.total_size, snap.plane_count,
                                snap.planes);
    }
    return rv;
}

// setSrcConfig -- non-drifted 5-arg form
int DpBlitStream::setSrcConfig(int w, int h, DP_COLOR_ENUM c,
                                DpInterlaceFormat il, DpRect* roi) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;
    using Fn = int (*)(void*, int, int, DP_COLOR_ENUM, DpInterlaceFormat, DpRect*);
    static Fn fn = reinterpret_cast<Fn>(resolve_blit(
        "_ZN12DpBlitStream12setSrcConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect"));
    if (!fn) return -1;
    sync_facade_to_real(facade, real);
    // N-era -> A11 color-format translation (see DpColorXlate.h).
    DP_COLOR_ENUM xc = static_cast<DP_COLOR_ENUM>(
        dpframework_compat::xlate_color_n_to_a(static_cast<int32_t>(c)));
    int rv = fn(real, w, h, xc, il, roi);
    sync_real_to_facade(facade, real);
    return rv;
}

// setDstConfig -- non-drifted 5-arg form
int DpBlitStream::setDstConfig(int w, int h, DP_COLOR_ENUM c,
                                DpInterlaceFormat il, DpRect* roi) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;
    using Fn = int (*)(void*, int, int, DP_COLOR_ENUM, DpInterlaceFormat, DpRect*);
    static Fn fn = reinterpret_cast<Fn>(resolve_blit(
        "_ZN12DpBlitStream12setDstConfigEii13DP_COLOR_ENUM17DpInterlaceFormatP6DpRect"));
    if (!fn) return -1;
    sync_facade_to_real(facade, real);
    // N-era -> A11 color-format translation (see DpColorXlate.h).
    DP_COLOR_ENUM xc = static_cast<DP_COLOR_ENUM>(
        dpframework_compat::xlate_color_n_to_a(static_cast<int32_t>(c)));
    int rv = fn(real, w, h, xc, il, roi);
    sync_real_to_facade(facade, real);
    return rv;
}

// setSrcBuffer (fd-based, 3-arg)
int DpBlitStream::setSrcBuffer(int fd, unsigned int* pSizeList,
                                unsigned int planeCount) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;
    using Fn = int (*)(void*, int, unsigned int*, unsigned int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_ZN12DpBlitStream12setSrcBufferEiPjj"));
    if (!fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = fn(real, fd, pSizeList, planeCount);
    sync_real_to_facade(facade, real);
    return rv;
}

// setSrcBuffer (va, 2-arg) -- VA->ION memcpy bridge
// ======================================================
// A11 rejects VA inputs at DpIonHandler::mapHWAddress ("not support alloc
// by va"). We allocate a fresh ION buffer, memcpy VA data in, ion_sync_fd
// for cache coherency, then forward to A11's FD-form setSrcBuffer which
// routes through registerBufferFD -> fd-mode DpIonHandler (accepted).
//
// Fallback: if libion isn't available or alloc fails, we forward the VA
// form unchanged. That will fail at A11 the same way it did before the
// shim -- no new crash class introduced (fail-closed for HAL survival).
int DpBlitStream::setSrcBuffer(void* va, unsigned int size) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;

    // Resolve both A11 forms once.
    using VaFn = int (*)(void*, void*, unsigned int);
    using FdFn = int (*)(void*, int, unsigned int*, unsigned int);
    static VaFn va_fn = reinterpret_cast<VaFn>(
        resolve_blit("_ZN12DpBlitStream12setSrcBufferEPvj"));
    static FdFn fd_fn = reinterpret_cast<FdFn>(
        resolve_blit("_ZN12DpBlitStream12setSrcBufferEiPjj"));

    // Try the ION bridge first.
    int ion_fd = -1;
    if (fd_fn && va && size > 0) {
        ion_fd = ion_bridge_alloc_and_copy(va, size);
    }

    if (ion_fd >= 0) {
        // Swap into shadow state; close the PREVIOUS fd (if any) outside the
        // shadow-map mutex to avoid holding a lock across a syscall.
        int prev_fd = blit_shadow_swap_src_ion_fd(facade, ion_fd);
        ion_bridge_close(prev_fd);

        sync_facade_to_real(facade, real);
        unsigned int sizes[1] = { size };
        int rv = fd_fn(real, ion_fd, sizes, 1u);
        sync_real_to_facade(facade, real);
        return rv;
    }

    // Fallback path -- no ION bridge available. Forward the VA form (will fail
    // at A11 with "not support alloc by va" but no crash).
    if (!va_fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = va_fn(real, va, size);
    sync_real_to_facade(facade, real);
    return rv;
}

// setSrcBuffer (va-array 3-arg) -- multi-plane VA->ION bridge.
//
// The hot path in
// libfeatureio's doRGB565Buffer_DDP / doYV12Buffer_DDP dispatches via a
// 3-way platform-flag switch where flags 2/3 select this multi-plane form.
// The 2-arg bridge alone did not reduce the VA rejection rate, consistent
// with the platform flag routing to this form at runtime.
//
// Strategy: allocate ONE ION buffer sized sum(pSizeList[]), memcpy each
// plane in at its offset, sync, forward to A11's FD form. The FD form
// (registerBufferFD @0x13f3c) stores the same fd to one stack slot per
// plane when planeCount in {1,2,3} -- that's exactly "one dma-buf holding
// N planes" convention.
int DpBlitStream::setSrcBuffer(void** pVaList, unsigned int* pSizeList,
                                unsigned int planeCount) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;

    using VaFn = int (*)(void*, void**, unsigned int*, unsigned int);
    using FdFn = int (*)(void*, int, unsigned int*, unsigned int);
    static VaFn va_fn = reinterpret_cast<VaFn>(
        resolve_blit("_ZN12DpBlitStream12setSrcBufferEPPvPjj"));
    static FdFn fd_fn = reinterpret_cast<FdFn>(
        resolve_blit("_ZN12DpBlitStream12setSrcBufferEiPjj"));

    int ion_fd = -1;
    size_t total = 0;
    size_t offsets[3] = {0, 0, 0};
    if (fd_fn && pVaList && pSizeList && planeCount >= 1 && planeCount <= 3) {
        ion_fd = ion_bridge_alloc_and_copy_multi(pVaList, pSizeList, planeCount,
                                                  &total, offsets);
    }

    if (ion_fd >= 0) {
        int prev_fd = blit_shadow_swap_src_ion_fd(facade, ion_fd);
        ion_bridge_close(prev_fd);

        sync_facade_to_real(facade, real);
        int rv = fd_fn(real, ion_fd, pSizeList, planeCount);
        sync_real_to_facade(facade, real);
        return rv;
    }

    // Fallback -- forward VA form unchanged. Will hit A11 "not support
    // alloc by va" but no new crash class.
    if (!va_fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = va_fn(real, pVaList, pSizeList, planeCount);
    sync_real_to_facade(facade, real);
    return rv;
}

// setDstBuffer (fd-based, 3-arg)
int DpBlitStream::setDstBuffer(int fd, unsigned int* pSizeList,
                                unsigned int planeCount) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;
    using Fn = int (*)(void*, int, unsigned int*, unsigned int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_ZN12DpBlitStream12setDstBufferEiPjj"));
    if (!fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = fn(real, fd, pSizeList, planeCount);
    sync_real_to_facade(facade, real);
    return rv;
}

// setDstBuffer (va, 2-arg) -- VA->ION bridge with readback-on-invalidate.
//
// Allocates an ION buffer of `size`, forwards FD form. Saves the caller's VA
// + fd as a pending dst readback descriptor in shadow state. At invalidate()
// (when HW is guaranteed done), the readback mmaps the fd, copies ION bytes
// back into the caller's VA, and munmaps.
int DpBlitStream::setDstBuffer(void* va, unsigned int size) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;

    using VaFn = int (*)(void*, void*, unsigned int);
    using FdFn = int (*)(void*, int, unsigned int*, unsigned int);
    static VaFn va_fn = reinterpret_cast<VaFn>(
        resolve_blit("_ZN12DpBlitStream12setDstBufferEPvj"));
    static FdFn fd_fn = reinterpret_cast<FdFn>(
        resolve_blit("_ZN12DpBlitStream12setDstBufferEiPjj"));

    int ion_fd = -1;
    if (fd_fn && va && size > 0) {
        ion_fd = ion_bridge_alloc_only(size);
    }

    if (ion_fd >= 0) {
        DstReadback planes[3];
        planes[0] = DstReadback{ va, size, 0 };
        planes[1] = DstReadback{ nullptr, 0, 0 };
        planes[2] = DstReadback{ nullptr, 0, 0 };
        int prev_fd = blit_shadow_install_dst(facade, ion_fd, size, 1, planes);
        ion_bridge_close(prev_fd);

        sync_facade_to_real(facade, real);
        unsigned int sizes[1] = { size };
        int rv = fd_fn(real, ion_fd, sizes, 1u);
        sync_real_to_facade(facade, real);
        return rv;
    }

    // Fallback: forward VA form (will fail at A11, no crash).
    if (!va_fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = va_fn(real, va, size);
    sync_real_to_facade(facade, real);
    return rv;
}

// setDstBuffer (va-array 3-arg) -- multi-plane VA->ION dst bridge.
int DpBlitStream::setDstBuffer(void** pVaList, unsigned int* pSizeList,
                                unsigned int planeCount) {
    void* facade = this;
    void* real = blit_shadow_lookup(facade);
    if (!real) return -1;

    using VaFn = int (*)(void*, void**, unsigned int*, unsigned int);
    using FdFn = int (*)(void*, int, unsigned int*, unsigned int);
    static VaFn va_fn = reinterpret_cast<VaFn>(
        resolve_blit("_ZN12DpBlitStream12setDstBufferEPPvPjj"));
    static FdFn fd_fn = reinterpret_cast<FdFn>(
        resolve_blit("_ZN12DpBlitStream12setDstBufferEiPjj"));

    int ion_fd = -1;
    size_t total = 0;
    size_t offsets[3] = {0, 0, 0};
    if (fd_fn && pVaList && pSizeList && planeCount >= 1 && planeCount <= 3) {
        // Compute total size first (no memcpy for dst -- HW writes INTO it).
        for (unsigned int i = 0; i < planeCount; ++i) {
            offsets[i] = total;
            total += pSizeList[i];
        }
        if (total > 0) {
            ion_fd = ion_bridge_alloc_only(total);
        }
    }

    if (ion_fd >= 0) {
        DstReadback planes[3];
        for (int i = 0; i < 3; ++i) {
            if ((unsigned int)i < planeCount) {
                planes[i] = DstReadback{ pVaList[i], pSizeList[i], offsets[i] };
            } else {
                planes[i] = DstReadback{ nullptr, 0, 0 };
            }
        }
        int prev_fd = blit_shadow_install_dst(facade, ion_fd, total,
                                              (int)planeCount, planes);
        ion_bridge_close(prev_fd);

        sync_facade_to_real(facade, real);
        int rv = fd_fn(real, ion_fd, pSizeList, planeCount);
        sync_real_to_facade(facade, real);
        return rv;
    }

    if (!va_fn) return -1;
    sync_facade_to_real(facade, real);
    int rv = va_fn(real, pVaList, pSizeList, planeCount);
    sync_real_to_facade(facade, real);
    return rv;
}

// ---------------------------------------------------------------------------
// Non-class free-function forwarder.
// libimageio_plat_drv_FrmB.so imports this to size an internal working
// buffer.
// ---------------------------------------------------------------------------

extern "C" int _Z45tpipe_main_query_platform_working_buffer_sizei(int arg);
int _Z45tpipe_main_query_platform_working_buffer_sizei(int arg) {
    using Fn = int (*)(int);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_Z45tpipe_main_query_platform_working_buffer_sizei"));
    if (!fn) return 0;
    return fn(arg);
}

// ---------------------------------------------------------------------------
// A11-form re-exports for libdpframework_n_era_shim.so.
// n_era_shim DT_NEEDEDs libdpframework_compat.so and
// forwards N-era -> A11 internally; for that to resolve, compat must
// export the A11 forms too. Simple dlsym forwarders -- they do NOT touch
// the shadow map (they're called with A11-form `this` from n_era_shim,
// which was set up against real A11 objects, not facades).
// ---------------------------------------------------------------------------

extern "C" {

int _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure(
    void* thisp, int w, int h, int pitch, int scanline,
    int color, int profile, int il, void* roi, bool flip, int secure)
{
    using Fn = int (*)(void*, int, int, int, int, int, int, int, void*, bool, int);
    static Fn fn = reinterpret_cast<Fn>(resolve_blit(
        "_ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (!fn) return -1;
    // N-era -> A11 color-format translation; callers still carry N-era codes
    // even on the A11-form re-export path (defence-in-depth).
    int xcolor = dpframework_compat::xlate_color_n_to_a(color);
    return fn(thisp, w, h, pitch, scanline, xcolor, profile, il, roi, flip, secure);
}

int _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure(
    void* thisp, int port, int w, int h, int pitch, int scanline,
    int color, int profile, int il, void* roi, bool flip, int secure)
{
    using Fn = int (*)(void*, int, int, int, int, int, int, int, int, void*, bool, int);
    static Fn fn = reinterpret_cast<Fn>(resolve_blit(
        "_ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure"));
    if (!fn) return -1;
    // N-era -> A11 color-format translation (defence-in-depth).
    int xcolor = dpframework_compat::xlate_color_n_to_a(color);
    return fn(thisp, port, w, h, pitch, scanline, xcolor, profile, il, roi, flip, secure);
}

int _ZN11DpIspStream11startStreamEP7timeval(void* thisp, struct timeval* tv)
{
    using Fn = int (*)(void*, struct timeval*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_ZN11DpIspStream11startStreamEP7timeval"));
    if (!fn) return -1;
    return fn(thisp, tv);
}

int _ZN12DpBlitStream10invalidateEP7timeval(void* thisp, struct timeval* tv)
{
    using Fn = int (*)(void*, struct timeval*);
    static Fn fn = reinterpret_cast<Fn>(
        resolve_blit("_ZN12DpBlitStream10invalidateEP7timeval"));
    if (!fn) return -1;
    return fn(thisp, tv);
}

}  // extern "C"
