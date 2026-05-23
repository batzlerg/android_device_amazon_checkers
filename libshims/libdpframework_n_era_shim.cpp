// libdpframework_n_era_shim.cpp
// Provides N-era DpIspStream and DpBlitStream symbols missing from A11 libdpframework.so.
//
// Exported (N-era forms, what importing blobs call):
//   _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb
//     DpIspStream::setSrcConfig(int,int,int,int,DP_COLOR_ENUM,DP_PROFILE_ENUM,DpInterlaceFormat,DpRect*,bool)
//   _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb
//     DpIspStream::setDstConfig(int,int,int,int,int,DP_COLOR_ENUM,DP_PROFILE_ENUM,DpInterlaceFormat,DpRect*,bool)
//   _ZN11DpIspStream11startStreamEv
//     DpIspStream::startStream()
//   _ZN12DpBlitStream10invalidateEv
//     DpBlitStream::invalidate()
//
// Imported (A11 forms, resolved from on-device libdpframework.so at load time):
//   _ZN11DpIspStream12setSrcConfigEiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream12setDstConfigEiiiii13DP_COLOR_ENUM15DP_PROFILE_ENUM17DpInterlaceFormatP6DpRectb8DpSecure
//   _ZN11DpIspStream11startStreamEP7timeval
//   _ZN12DpBlitStream10invalidateEP7timeval
//
// Forwarding strategy:
//   setSrcConfig/setDstConfig: append DpSecure=0 (DP_SECURE_NONE -- non-secure, stored as byte field,
//     no conditional branch in A11 implementation; safe for camera preview buffers)
//   startStream: pass stack-allocated timeval set to current time + 30s (A11 form dereferences
//     the pointer without null check; far-future deadline avoids expired-timeout early return)
//   DpBlitStream::invalidate: same timeval strategy as startStream -- current time + 30s deadline

#include <stdint.h>
#include <sys/time.h>

// Enum declarations -- must match Itanium mangling in the target symbol names.
enum DP_COLOR_ENUM : int { };
enum DP_PROFILE_ENUM : int { };
enum DpInterlaceFormat : int { };
enum DpSecure : int { DP_SECURE_NONE = 0 };

// Forward declaration -- DpRect* mangles to P6DpRect.
struct DpRect;

class DpIspStream {
public:
    // --- N-era exports (defined here, produce T symbols) ---

    int setSrcConfig(int width, int height, int pitch, int scanline,
                     DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                     DpInterlaceFormat interlace, DpRect* pROI, bool flip);

    int setDstConfig(int portIndex, int width, int height, int pitch, int scanline,
                     DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                     DpInterlaceFormat interlace, DpRect* pROI, bool flip);

    int startStream();

    // --- A11 imports (declared but not defined; produce U symbols resolved from
    //     on-device libdpframework.so via DT_NEEDED added by patchelf post-build) ---

    int setSrcConfig(int width, int height, int pitch, int scanline,
                     DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                     DpInterlaceFormat interlace, DpRect* pROI, bool flip,
                     DpSecure secure);

    int setDstConfig(int portIndex, int width, int height, int pitch, int scanline,
                     DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                     DpInterlaceFormat interlace, DpRect* pROI, bool flip,
                     DpSecure secure);

    int startStream(struct timeval* pWaitTimeout);
};

class DpBlitStream {
public:
    // --- N-era export (defined here, produces T symbol) ---
    int invalidate();

    // --- A11 import (declared but not defined; produces U symbol resolved from
    //     on-device libdpframework.so via DT_NEEDED) ---
    int invalidate(struct timeval* pWaitTimeout);
};

// N-era 9-arg setSrcConfig -> A11 10-arg setSrcConfig (DpSecure=0)
int DpIspStream::setSrcConfig(int width, int height, int pitch, int scanline,
                               DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                               DpInterlaceFormat interlace, DpRect* pROI, bool flip) {
    return setSrcConfig(width, height, pitch, scanline,
                        color, profile, interlace, pROI, flip,
                        DP_SECURE_NONE);
}

// N-era 10-arg setDstConfig -> A11 11-arg setDstConfig (DpSecure=0)
int DpIspStream::setDstConfig(int portIndex, int width, int height, int pitch, int scanline,
                               DP_COLOR_ENUM color, DP_PROFILE_ENUM profile,
                               DpInterlaceFormat interlace, DpRect* pROI, bool flip) {
    return setDstConfig(portIndex, width, height, pitch, scanline,
                        color, profile, interlace, pROI, flip,
                        DP_SECURE_NONE);
}

// N-era no-arg startStream -> A11 startStream(timeval*)
// Pass a 30-second future deadline. A11 dereferences the pointer without null check.
int DpIspStream::startStream() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec += 30;
    return startStream(&tv);
}

// N-era no-arg DpBlitStream::invalidate -> A11 invalidate(timeval*)
// Same strategy as startStream: 30-second future deadline.
int DpBlitStream::invalidate() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    tv.tv_sec += 30;
    return invalidate(&tv);
}
