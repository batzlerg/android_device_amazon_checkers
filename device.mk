#
# SPDX-FileCopyrightText: 2025 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

$(call inherit-product, frameworks/native/build/tablet-7in-hdpi-1024-dalvik-heap.mk)

# AAPT
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := hdpi

# Camera HAL -- camerahalserver (AOSP 8.1 legacy HIDL wrapper for stock MTK camera.mt8163.so)
# Bridges N-era MediaTek camera HAL to Android 11 android.hardware.camera.provider@2.4 HIDL.
PRODUCT_PACKAGES += \
    camerahalserver \
    android.hardware.camera.provider@2.4-impl-checkers

# Camera shims -- N-era -> A11 symbol bridge libraries
# libui_n_era_shim:         GraphicBuffer::lock 2-arg, 7-arg ctor, GraphicBufferMapper::lock 4-arg
# libdpframework_n_era_shim: DpIspStream setSrcConfig/setDstConfig/startStream + DpBlitStream::invalidate
# libdpframework_compat:    DpIspStream shadow-alloc ABI compat; fixes 0x5a8->0x4800 struct-size
#                           drift in the five N-era caller blobs. Subsumes n_era_shim's DpIspStream
#                           handling; n_era_shim is still needed for DpBlitStream::invalidate.
# All added to importing blobs via blob_fixup() in extract-files.sh.
PRODUCT_PACKAGES += \
    libui_n_era_shim \
    libdpframework_n_era_shim \
    libdpframework_compat


# Audio
PRODUCT_PACKAGES += \
    android.hardware.audio@2.0-impl \
    android.hardware.audio.effect@2.0-impl \
    audio.primary.amazon_wrapper

PRODUCT_PACKAGES += \
    libaudio-resampler \
    libaudioutils \
    libaudioroute \
    libtinyalsa \
    libamazonlog

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/audio_policy_configuration.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_policy_configuration.xml

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-service \
    android.hardware.bluetooth@1.0-impl \
    libbt-vendor

PRODUCT_PACKAGES += \
    mt76x8_bt

# Init
PRODUCT_PACKAGES += \
    init.target.checkers.rc

# Input
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/configs/idc/,$(TARGET_COPY_OUT_VENDOR)/usr/idc/)

# Modules
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/init.insmod.cfg:$(TARGET_COPY_OUT_VENDOR)/etc/init.insmod.cfg

# Overlays
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

PRODUCT_PACKAGES += \
    FrameworksResOverlayCheckers \
    SystemUIOverlayCheckers

# Screen
TARGET_SCREEN_DENSITY := 195
TARGET_SCREEN_HEIGHT := 480
TARGET_SCREEN_WIDTH := 960

# Sensors
PRODUCT_PACKAGES += \
    sensors.amazon

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml

# Shipping API Level
PRODUCT_SHIPPING_API_LEVEL := 25

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Suspend blocker
PRODUCT_PACKAGES += \
    suspend_blocker_mt8163

# Thermal
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/thermal.policy.conf:$(TARGET_COPY_OUT_VENDOR)/etc/.tp/thermal.policy.conf

# Vendor partition
TARGET_HAS_VENDOR_PARTITION := false

# Wi-Fi
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service-lazy

PRODUCT_PACKAGES += \
    mt76x8_wlan

PRODUCT_PACKAGES += \
    WifiResOverlayCheckers

PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/configs/wifi/,$(TARGET_COPY_OUT_VENDOR)/etc/wifi)

# Inherit from mt8163-common
$(call inherit-product, device/amazon/mt8163-common/mt8163.mk)

# Inherit the proprietary files
$(call inherit-product, vendor/amazon/checkers/checkers-vendor.mk)
