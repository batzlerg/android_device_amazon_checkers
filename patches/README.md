# AOSP-tree patches for checkers camera HAL

This directory contains patches that must be applied to AOSP sub-repos
(`hardware/interfaces`, `frameworks/av`, `frameworks/base`, `packages/apps/Camera2`)
after every `repo sync` that resets those sub-repos.

## Why patches instead of a fork?

Forking `hardware/interfaces` is a possible future direction. The patch mechanism is the durable interim story:
- Patches are version-controlled here in the device tree
- `apply-patches.sh` re-applies them idempotently after any `repo sync`
- The patch can be converted to a fork at any time by committing the working tree

## How to apply

```bash
# From the LineageOS root:
bash device/amazon/checkers/patches/apply-patches.sh
```

The script is idempotent: if a patch is already applied (reverse-check succeeds),
it skips it with a "already applied" message.

## Patch inventory

### hardware-interfaces-camera-device-camera1-bridge.patch

**Target:** `hardware/interfaces/`

**Files:** `camera/device/1.0/default/CameraDevice.cpp`,
`camera/device/1.0/default/CameraDevice_1_0.h`,
`camera/device/3.2/default/CameraDevice.cpp`

This single consolidated patch reproduces all four Camera1-bridge fixes.

---

**Fix 1 -- `sCameraInstance` NULL-callback guard**
(`camera/device/1.0/default/CameraDevice.cpp` + `CameraDevice_1_0.h`)

**Problem:** The A11 HIDL camera device bridge (`camera.device@1.0-impl`) unconditionally
casts the `user` context pointer from N-era HAL callbacks (`get_memory`, `notify`,
`data`, `data_timestamp`) to `CameraDevice*` to recover the instance for HIDL memory
registration. The stock MTK N-era `camera.mt8163.so` passes `user=NULL` in these
callbacks (matching the N-era `CameraHardwareInterface` design where `user` was
`__attribute__((unused))`). This caused a null-pointer dereference at offset `0x8c`
(the `mDeviceCallback` field) -- `tombstone_47`, fault addr `0x8c`, `SEGV_MAPERR`.

**Fix:** Added a `static CameraDevice* sCameraInstance` fallback to `CameraDevice_1_0.h`.
Set in `CameraDevice::open()` before `set_callbacks`, cleared in `closeLocked()`.
All four static callbacks (`sGetMemory`, `sNotifyCb`, `sDataCb`, `sDataCbTimestamp`)
fall back to `sCameraInstance` when `user == nullptr`.

**Safe for:** Single-camera MT8163/OV9734 architecture on LineageOS 18.1 (checkers).
Not suitable for multi-camera devices without additional locking.

**Outcome:** `sGetMemory` crash eliminated. `startPreview` progresses past callback
wiring into the MDP pipeline.

---

**Fix 2 -- EXIF Orientation rewrite**
(`camera/device/1.0/default/CameraDevice.cpp`)

**Problem:** The N-era MTK HAL encodes the JPEG with EXIF Orientation=1 (no rotation),
requiring consumers to know the sensor mounting angle out-of-band. On a device where the
compositor-level display rotation is invisible to the framework's `Display.getRotation()`
path, every consumer that derives orientation from that API writes the wrong EXIF tag.

**Fix:** `patchJpegExifOrientation()` in `sDataCb`, gated on sysprop
`persist.camera.hal.exif_orientation`. Walks JPEG APP1 markers, navigates IFD0, locates
tag `0x0112`, and overwrites the inline SHORT value with the sysprop-specified value.
When the sysprop is unset, the callback is a no-op and the vendor JPEG passes through
unchanged.

**Safe for:** Any single-camera device that emits a complete JPEG with a standard
APP1/IFD0 block. The walk is defensive: if any marker or tag is absent the callback
returns without modification.

**Outcome:** Captured JPEGs carry EXIF Orientation=3 (required for the Camera2
passthrough patch to save the correct orientation and for gallery3d to render captures
upright). Verified end-to-end on this device.

---

**Fix 3 -- Sensor-native size filter**
(`camera/device/1.0/default/CameraDevice.cpp`)

**Problem:** The N-era MTK HAL advertises multiple preview and picture sizes, including
sub-native resolutions (320x240, 640x480, 1024x768) that the Camera2 app selects by
default. Sub-native sizes use the same sensor pipeline and produce lower-quality output
with no benefit on this hardware.

**Fix:** `getParameters` filter gated on sysprop
`persist.camera.hal.supported_sizes_filter=native_only`. When active, rewrites both
`picture-size-values` and `preview-size-values` in the parameter string to expose only
the sensor-native maximum (1280x720). When the sysprop is unset or set to any other
value the filter is a no-op and the full advertised list passes through.

**Safe for:** Single-sensor devices where a definitive "native maximum" exists. Not
suitable for multi-sensor or zoom-capable devices without extension.

**Outcome:** AOSP Camera2 defaults to 1280x720 for both preview and capture; Open Camera
already selects the largest advertised size by policy, so it is unaffected.

---

**Fix 4 -- `register_stream_buffers` zeroed post-open**
(`camera/device/3.2/default/CameraDevice.cpp`)

**Problem:** The N-era MTK HAL populates the `register_stream_buffers` slot in its
`camera3_device_ops` table. Android 11 cameraserver requires this entry point to be NULL
for `CAMERA_DEVICE_API_VERSION_3_2` and later (it was removed from the HAL3 contract); a
non-NULL pointer fails cameraserver's ops-table validation and session configuration is
rejected.

**Fix:** After `open()` and before `createSession()`, set
`device->ops->register_stream_buffers = nullptr` (guarded by a `device->ops != nullptr`
check). The bridge thus presents a HAL3.2-conformant ops table to cameraserver
regardless of what the vendor HAL populated.

**Safe for:** Any HAL whose ops table predates the HAL3.2 removal of
`register_stream_buffers`. Zeroing the slot is correct for all 3.2+ consumers because
the framework no longer invokes this entry point.

### packages-apps-Camera2-jpeg-passthrough.patch

**Target:** `packages/apps/Camera2/`

**Files:** `src/com/android/camera/processing/imagebackend/TaskCompressImageToJpeg.java`

**Problem -- JPEG re-crop on a sensor-aligned input.** `TaskCompressImageToJpeg`
treats the encoded JPEG as already display-aligned: it rotates the sensor-coordinate
crop into JPEG-native space via `rotateBoundingBox`, then clamps with
`guaranteedSafeCrop`. When the camera reports a non-trivial EXIF Orientation tag the
JPEG bytes are *not* pre-rotated -- the rotation is metadata applied at display time --
so rotating the crop double-rotates and a full-frame landscape sensor crop gets
clipped to a square (e.g. 1280x720 -> 720x720 at CLOCKWISE_90). The full-image
short-circuit also misses, because `requiresCropOperation` queries the BLOB
`ImageReader`'s 1-D buffer dimensions (byte count and 1) which never equal a 2-D
pixel rectangle, so `decompressCropAndRecompressJpegData` runs on every still
capture.

**Fix:** When `exifDerivedRotation` is non-trivial, apply the sensor-coordinate
crop directly without rotating it into JPEG-native space. Add a full-image
short-circuit before `requiresCropOperation` so a `safeCrop` covering the full
EXIF-declared image takes the existing pass-through branch (no decompress, no
recompress, no EXIF rewrite).

**Safe for:** Any camera that emits a complete JPEG with valid EXIF
PixelXDimension/PixelYDimension and a non-trivial Orientation tag. The original
math runs unchanged when `exifDerivedRotation == CLOCKWISE_0`.

**Outcome:** Front-camera capture is full sensor frame with EXIF Orientation
honored by gallery3d (verified end-to-end on this device 2026-05-04). Eliminates the gallery3d intermittent-blank issue caused by the
recompression pipeline rewriting EXIF non-deterministically. The source-built
Camera2 APK is the deployed artifact. An earlier in-app
`getJpegCompressionQuality()` fallback hunk has been retired now that the
platform `CameraProfile` fix (`frameworks-base-CameraProfile-front-camera-fallback.patch`,
deployed via `framework.jar` + matching boot-image artifacts) returns a usable
quality on front-only devices.

### packages-apps-Camera2-captureLayoutHelper-remove-overlay-offset.patch

**Target:** `packages/apps/Camera2/`

**Files:** `src/com/android/camera/CaptureLayoutHelper.java`

**Problem -- broken wide-preview overlay branch.** `getPositionConfiguration()`
contains an `else if (previewAspectRatio > 14f / 9f)` branch (marked by the
author with `// TODO: This logic needs some refinement.`) that fires whenever
the preview is wider than 14:9. It right-aligns the preview against the
activity's right edge and lets the bottom bar overlay onto the preview. The
branch's design assumption -- that a wide-aspect preview is also wider than the
screen -- holds on Nexus-era narrow-ratio panels but fails on every modern
landscape display where the screen aspect exceeds the preview aspect. On this
device's 2.2:1 panel (960x433 drawable, 1.778 preview), the right-align math
piles ~190 px of letterbox on the left and overlays the bottom bar on the
preview's right edge.

**Fix:** Delete the branch entirely. The same input falls through to the
existing "Fit shorter edge" sub-branch, which sizes the preview to the
available height, gives the bottom bar its own lateral column, and prevents
overlap. The TODO is honestly resolved; no replacement logic is needed because
the fallback branches already cover the input space.

**Safe for:** Any device. The deleted branch was only ever reachable when the
preview aspect exceeded 14:9 AND the screen had lateral room to spare; on
narrower screens the earlier "fit longer edge" branch handles the same
condition. Removing the broken branch is a strict improvement.

**Outcome:** AOSP Camera2 preview on this device shifts from `(190, 0, 960, 433)`
right-aligned with overlay bar to `(73, 0, 843, 433)` left-aligned with the
shutter strip in its own lateral column at `(843, 0, 960, 433)`. The remaining
73 px black band on the left is leftover lateral space that AOSP's
`mBottomBarMaxHeight=120dp` cap prevents the shutter strip from absorbing;
eliminating it would require a wider change to the bar-sizing rule (out of
scope for this minimal upstream-pursuable fix). Validated end-to-end on
this device 2026-05-15.

### frameworks-base-CameraProfile-front-camera-fallback.patch

**Target:** `frameworks/base/`

**Files:** `media/java/android/media/CameraProfile.java`

**Problem:** `CameraProfile.getJpegEncodingQualityParameter(int quality)` (the
single-argument form) iterates the camera list looking for the first
back-facing camera and returns 0 when none exists. On a front-only device
(checkers / cronos / any single-front-camera Echo Show variant), every caller
of this widely-used framework API gets 0, which feeds
`Bitmap.compress(JPEG, 0, ...)` and produces an unusable image. AOSP's Camera2
app, the platform's screenshot pipeline, third-party Camera apps, and any
other client that trusts the API contract are all affected.

**Fix:** When no back-facing camera is found and the device has at least one
camera, fall back to the first available camera (by index 0). Returns 0 only
when no cameras exist at all, preserving the documented "no camera" return
value while making the function usable on front-only devices.

**Safe for:** Any device. The original behaviour is preserved when a
back-facing camera exists. The fallback is only consulted on devices where
the original behaviour would have returned 0 anyway.

**Outcome:** Any caller of `CameraProfile.getJpegEncodingQualityParameter` on
a front-only device gets a usable quality (>0), eliminating the need for
per-app workarounds. The previously-needed in-app fallback in
`packages/apps/Camera2/src/com/android/camera/processing/imagebackend/TaskCompressImageToJpeg.java::getJpegCompressionQuality()`
has been retired. Validated on this device 2026-05-04: capture without
the in-app fallback hunk produced a usable JPEG with EXIF=3 and recognizable
scene, confirming the framework-side fix returns a non-zero quality.

### frameworks-av-libcameraservice-Camera1-preview-transform-sysprop.patch

**Target:** `frameworks/av/`

**Files:** `services/camera/libcameraservice/device1/CameraHardwareInterface.cpp`,
`services/camera/libcameraservice/device1/CameraHardwareInterface.h`

**Problem:** On a device whose panel is rotated at the SurfaceFlinger compositor
level, that rotation is invisible to WindowManager, so the standard Camera1
`orientation -> preview-buffer transform` mapping computes the wrong
`NATIVE_WINDOW_SET_BUFFERS_TRANSFORM` value and the live preview is misrotated.
The correct transform also differs by preview consumer: a SurfaceTexture+GL
consumer (e.g. the AOSP Camera2 app) and a raw SurfaceView consumer (e.g. Open
Camera) need different transforms, so one static override cannot satisfy both.

**Fix:** Allow a sysprop to override the Camera1 preview-buffer transform, and
choose which sysprop to read by latching the consumer type from the first
`setPreviewTransform` call. A first call carrying
`NATIVE_WINDOW_TRANSFORM_ROT_90 | FLIP_H` (=5) marks a raw SurfaceView consumer
and routes to `persist.camera.preview_transform_raw`; any other first value marks
a GL consumer and routes to `persist.camera.preview_transform`. The hint is
sticky for the rest of the camera session. Both sysprops are no-ops when unset.

**Safe for:** Any device. With both sysprops unset the override path never fires
and behaviour is unchanged.

**Outcome:** Camera2 (GL) and Open Camera (raw) each get a correctly-rotated
preview from a single HAL build, selected automatically. Calibrated sysprop
values live in `device/amazon/checkers/vendor.prop`.
