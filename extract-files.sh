#!/bin/bash
#
# Copyright (C) 2016 The CyanogenMod Project
# Copyright (C) 2017-2020 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

function blob_fixup() {
    case "${1}" in
        vendor/lib/hw/audio.primary_amazon.mt8163.so)
            "${PATCHELF}" --add-needed "libamazonlog.so" "${2}"
            ;;
        vendor/lib/libaudiocomponentengine.so)
            "${PATCHELF}" --add-needed "libutilscallstack.so" "${2}"
            ;;

        # Camera HAL N-era -> A11 fixups.
        # These patchelf operations add the required shim DT_NEEDED entries to vendor blobs
        # at extract time, making them permanent without per-reflash adb fixups.

        # libcam_utils.so: needs libutilscallstack.so (CallStack symbols, no DT_NEEDED in blob)
        #   and libui_n_era_shim.so (2-arg GraphicBuffer::lock, 7-arg ctor -- removed in A11 libui).
        vendor/lib/libcam_utils.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --add-needed "libutilscallstack.so" "${2}"
            "${PATCHELF}" --add-needed "libui_n_era_shim.so" "${2}"
            ;;

        # libcam.utils.sensorlistener.so: needs libsensor.so (SensorEventQueue::enableSensor,
        #   no DT_NEEDED in blob -- vndksupport namespace isolation defeats RTLD_GLOBAL preloads).
        vendor/lib/libcam.utils.sensorlistener.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --add-needed "libsensor.so" "${2}"
            ;;

        # 5 blobs that import DpIspStream from A11 libdpframework.so where TWO distinct ABI
        # fix classes apply simultaneously:
        #   (1) DpIspStream struct-size drift: N-era callers allocate 0x5a8 bytes but
        #       A11 DpIspStream ctor writes up to ~0x46f8; overruns & SIGSEGVs.
        #       libdpframework_compat provides a shadow-allocation ABI shim that intercepts
        #       the ctor, allocates a larger real buffer, and thunks all 17 DpIspStream
        #       methods through a shadow map.
        #   (2) DpIspStream/DpBlitStream method signature drift:
        #       setSrcConfig(7-arg), setDstConfig(8-arg), startStream(no-arg),
        #       DpBlitStream::invalidate(no-arg). libdpframework_n_era_shim provides the
        #       N-era method forms. Compat subsumes DpIspStream symbols from n_era_shim;
        #       n_era_shim is still needed for DpBlitStream::invalidate.
        #
        # DT_NEEDED ORDER MATTERS. Compat must resolve BEFORE n_era_shim for the three
        # overlapping DpIspStream symbols (setSrcConfig/setDstConfig/startStream), so we
        # patchelf compat first. extract-files.sh is idempotent per extract, so ordering
        # is stable.
        vendor/lib/libimageio_plat_drv_FrmB.so \
        | vendor/lib/libimageio_plat_drv.so \
        | vendor/lib/libcam.campipe.so \
        | vendor/lib/libcam.iopipe.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --add-needed "libdpframework_compat.so" "${2}"
            "${PATCHELF}" --add-needed "libdpframework_n_era_shim.so" "${2}"
            ;;

        # libcam.camadapter.so: needs libdpframework_compat + libdpframework_n_era_shim
        #   (same as the 5 blobs above) for DpIspStream ABI compat and method-signature
        #   forwarding. DT_NEEDED order is preserved: compat wins GOT resolution for
        #   the three overlapping DpIspStream symbols.
        vendor/lib/libcam.camadapter.so)
            [ "$2" = "" ] && return 0
            "${PATCHELF}" --add-needed "libdpframework_compat.so" "${2}"
            "${PATCHELF}" --add-needed "libdpframework_n_era_shim.so" "${2}"
            ;;
    esac
}

# If we're being sourced by the common script that we called,
# stop right here. No need to go down the rabbit hole.
if [ "${BASH_SOURCE[0]}" != "${0}" ]; then
    return
fi

set -e

export DEVICE=checkers
export DEVICE_COMMON=mt8163-common
export VENDOR=amazon

"./../../${VENDOR}/${DEVICE_COMMON}/extract-files.sh" "$@"
