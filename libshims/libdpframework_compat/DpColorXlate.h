// libdpframework_compat -- N-era -> A11 DP_COLOR_ENUM translation table.
//
// PROBLEM
// =======
// A11 MediaTek re-encoded the upper 20 bits of DP_COLOR_ENUM (bpp / subsampling /
// plane count / raw-pack mode / AFBC / BLK metadata) between N-era Fire OS 6 and
// A11 Fire OS 7 MT8163 ports. The low 12 bits (format identity) are preserved.
// N-era caller blobs (libcam.camadapter, libcam.campipe, libcam.iopipe,
// libimageio_plat_drv{,_FrmB}) emit N-era DP_COLOR_ENUM literals that A11's
// DpEngine_WDMA::onConfigFrame and DpEngine_RDMA::onConfigFrame reject with
// "unsupported color format" -- 1310 hits/min, blocking preview and capture.
//
// FIX -- TRANSLATION TABLE
// =======================
// At each compat forwarder that takes a DP_COLOR_ENUM arg, look up the caller-
// supplied (N-era) code in this table; substitute the A11 equivalent before
// forwarding. Unknown codes pass through unchanged (defensive; already-A11
// codes or runtime-computed codes we didn't enumerate). A11 will still reject
// any unknown code that's neither N-era nor A11 -- same behavior as pre-fix.
//
// TABLE ENTRIES (17)
// ==================
// Each (N,A) pair verified by: presence in N-era WDMA+RDMA accept lists,
// presence in caller-blob MOVW/MOVT scans, presence in A11 accept lists with
// matching low-12-bit identity. Two N-era encodings collapse to the same A11
// target for RAW10 (0x1044, 0x1045) -- explicit dual entries.

#ifndef LIBDPFRAMEWORK_COMPAT_DPCOLORXLATE_H_
#define LIBDPFRAMEWORK_COMPAT_DPCOLORXLATE_H_

#include <stdint.h>

namespace dpframework_compat {

// Translate a DP_COLOR_ENUM code from N-era encoding to A11 encoding.
// Passthrough for codes not in the table (unknown, already-A11, or not yet
// enumerated). Operates on the full 32-bit value; the low 12 bits carry the
// color identity, the upper 20 bits carry the metadata re-encoded between
// eras.
//
// Implementation: a linear switch on the 17 known mappings. At this table
// size a linear switch compiles to a jump-table / tree the compiler
// optimizes itself; no unordered_map overhead, and the shim can avoid
// std::unordered_map's static-init-order hazards that previously bit us.
static inline int32_t xlate_color_n_to_a(int32_t c) {
    switch (c) {
        // Low 12 bits preserved; upper 20 bits re-encoded.
        case 0x01000847: return 0x00200847;  // low:0x847
        case 0x01001000: return 0x00201000;  // low:0x000
        case 0x01001821: return 0x00201821;  // low:0x821
        case 0x01002002: return 0x00202002;  // low:0x002
        case 0x01002003: return 0x00202003;  // low:0x003
        case 0x01002022: return 0x00202022;  // low:0x022
        case 0x01002023: return 0x00202023;  // low:0x023
        case 0x01101044: return 0x00281044;  // low:0x044 (RAW10)
        case 0x11010044: return 0x00281044;  //   N-era 2-encoding collapse
        case 0x01101045: return 0x00281045;  // low:0x045
        case 0x11010045: return 0x00281045;  //   N-era 2-encoding collapse
        case 0x01101064: return 0x00281064;  // low:0x064 (RAW12)
        case 0x01101065: return 0x00281065;  // low:0x065
        case 0x0254084c: return 0x004c084c;  // low:0x84c (NV12 dual-plane)
        case 0x0254086c: return 0x004c086c;  // low:0x86c (NV21 dual-plane)
        case 0x03140848: return 0x006c0848;  // low:0x848 (YUV420 3-plane I420)
        case 0x03140868: return 0x006c0868;  // low:0x868 (YUV420 3-plane YV12)
        default:         return c;           // passthrough (unknown / already-A11)
    }
}

}  // namespace dpframework_compat

#endif  // LIBDPFRAMEWORK_COMPAT_DPCOLORXLATE_H_
