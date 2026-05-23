/*
 * Copyright (C) 2016 The Android Open Source Project
 * Modifications copyright (C) 2026 graham <batzler@pm.me>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * camerahalserver -- service entry point for checkers (Amazon Echo Show 5 Gen 1, MT8163).
 * Forked from AOSP 8.1 hardware/interfaces/camera/provider/2.4/default/service.cpp,
 * adapted for Android 11 HIDL and the checkers/MT8163 N-era camera blob closure.
 *
 * Key difference from upstream service.cpp:
 *
 *   Mandatory RTLD_GLOBAL preload sequence before hw_get_module() is called.
 *   The stock N-era MTK camera blobs (camera.mt8163.so and its dep chain) have
 *   implicit dependencies on symbols from 4 libraries that are NOT expressed via
 *   DT_NEEDED entries.  The Android dynamic linker cannot resolve these lazily;
 *   they must be globally visible before any MTK camera blob is loaded.
 *
 *   Load order is MANDATORY (empirically validated):
 *     1. libutilscallstack.so   -- CallStack symbols used by libcam_utils.so
 *     2. libsensor.so           -- SensorEventQueue::enableSensor(Sensor const*)
 *                                 used by libcam.utils.sensorlistener.so dep chain
 *     3. libui_n_era_shim.so    -- N-era GraphicBuffer/GraphicBufferMapper symbols
 *                                 absent from Android 11 libui.so
 *     4. libcamera_client_mtk_shim.so -- 4 MTK-proprietary MtkCameraParameters /
 *                                       MtkCamUtils::DevMetaInfo symbols absent
 *                                       from Android 11 libcamera_client.so
 *
 *   camera.mt8163.so is loaded implicitly when defaultPassthroughServiceImplementation
 *   triggers the passthrough impl library to call hw_get_module(CAMERA_HARDWARE_MODULE_ID).
 *
 * The preload sequence was validated empirically: any permutation that reorders
 * or omits a preload causes symbol resolution failures at camera HAL open time.
 */

#define LOG_TAG "camerahalserver"

#include <android/hardware/camera/provider/2.4/ICameraProvider.h>
#include <hidl/LegacySupport.h>
#include <binder/ProcessState.h>
#include <android/log.h>
#include <dlfcn.h>

using android::hardware::camera::provider::V2_4::ICameraProvider;
using android::hardware::defaultPassthroughServiceImplementation;

int main() {
    ALOGI("camerahalserver: starting (Echo Show 5 Gen 1 / checkers / MT8163)");

    /*
     * Step 1: RTLD_GLOBAL preloads.
     *
     * These 4 libraries must be globally visible in the process linker namespace
     * BEFORE hw_get_module("camera") loads camera.mt8163.so and its dep chain.
     * Loading order matches the dependency graph (leaf -> root):
     *   libutilscallstack -> libsensor -> libui_n_era_shim -> libcamera_client_mtk_shim
     *
     * RTLD_NOW is used so any missing symbols in the preload libs surface here
     * as explicit failures rather than silent lazy-resolution failures later.
     */
    static const char* kPreloadLibs[] = {
        "/system/lib/libutilscallstack.so",
        "/system/lib/libsensor.so",
        "/vendor/lib/libui_n_era_shim.so",
        "/vendor/lib/libcamera_client_mtk_shim.so",
    };

    for (const char* lib : kPreloadLibs) {
        void* handle = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            ALOGE("camerahalserver: FATAL: dlopen(%s) failed: %s -- cannot continue", lib, dlerror());
            return 1;
        }
        ALOGI("camerahalserver: preloaded %s OK", lib);
    }

    ALOGI("camerahalserver: all 4 preloads OK -- registering ICameraProvider/legacy/0");

    /*
     * Step 2: Bind libbinder to /dev/binder for system-service lookups.
     *
     * Checkers (legacy single-partition device, non-Treble ld.config) does not
     * register camera-relevant services on /dev/vndbinder. MTK sensor-listener
     * path (libcam.utils.sensorlistener.so -> SensorManager::getInstanceForPackage
     * -> libbinder waitForService("sensorservice")) needs /dev/binder to find
     * the system sensorservice. (Prior form used /dev/vndbinder; caused waitForService("sensorservice")
     * to spin indefinitely in the camera preview thread.)
     *
     * HIDL registration goes through /dev/hwbinder regardless of this setting,
     * so it is unaffected. Runtime evidence: `vndservice list` on checkers shows
     * only the service-manager interface itself on /dev/vndbinder, so no vendor
     * service of interest is lost by this switch.
     */
    android::ProcessState::initWithDriver("/dev/binder");

    /*
     * Step 3: Register android.hardware.camera.provider@2.4::ICameraProvider/legacy/0.
     *
     * Android 11 CameraProviderManager enforces that the provider instance name must
     * match the device-type prefix: legacy HAL1 devices are enumerated as
     * "device@1.0/legacy/N", so the provider MUST be named "legacy/0".
     * The previous "internal/0" registration caused EINVAL in addDevice:
     *   "Device type legacy does not match provider type internal"
     *
     * The on-device VINTF manifest (/vendor/etc/vintf/manifest.xml) must declare
     * "legacy/0" to match.
     *
     * defaultPassthroughServiceImplementation will dlopen the passthrough impl library
     * (android.hardware.camera.provider@2.4-impl-checkers.so), call HIDL_FETCH_ICameraProvider("legacy/0"),
     * and register the returned instance as the binderized "legacy/0" provider.
     * The impl library itself calls hw_get_module() which loads camera.mt8163.so -- at that
     * point all 4 preloaded libs are already globally visible.
     */
    return defaultPassthroughServiceImplementation<ICameraProvider>(
            "legacy/0", /*maxThreads*/ 6);
}
