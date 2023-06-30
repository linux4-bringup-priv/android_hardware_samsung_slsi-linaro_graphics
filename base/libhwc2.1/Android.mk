# Copyright (C) 2012 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

LOCAL_PATH:= $(call my-dir)
# HAL module implemenation, not prelinked and stored in
# hw/<COPYPIX_HARDWARE_MODULE_ID>.<ro.product.board>.so

include $(CLEAR_VARS)

ifndef TARGET_SOC_BASE
	TARGET_SOC_BASE := $(TARGET_SOC)
endif

LOCAL_SHARED_LIBRARIES := liblog libcutils libhardware \
	libhardware_legacy libutils libsync libacryl libui libion_exynos libion libexynosgraphicbuffer libdrmresource libdrm \

LOCAL_SHARED_LIBRARIES += \
	libprocessgroup

LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers libexynos_headers
LOCAL_STATIC_LIBRARIES += libVendorVideoApi
LOCAL_PROPRIETARY_MODULE := true

LOCAL_HEADER_LIBRARIES += libhdrinterface_header libhdr10p_meta_interface_header
ifdef BOARD_LIBHDR_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR_PLUGIN)
endif
ifdef BOARD_LIBHDR10P_META_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR10P_META_PLUGIN)
endif

ifeq ($(BOARD_USES_DQE_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdqeInterface
LOCAL_HEADER_LIBRARIES += libdqeInterface_headers
endif

ifeq ($(BOARD_USES_DISPLAY_COLOR_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdisplaycolor_default
LOCAL_HEADER_LIBRARIES += libdisplaycolor_interface
endif

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/driver_header \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1 \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/libhwcService \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libdrmresource

LOCAL_SRC_FILES := \
	device/ExynosDevice.cpp \
	device/ExynosDeviceDrmInterface.cpp \
	device/ExynosDeviceFbInterface.cpp \
	device/ExynosDeviceInterface.cpp \
	device/ExynosResourceManager.cpp \
	display/ExynosDisplay.cpp \
	display/ExynosDisplayDrmInterface.cpp \
	display/ExynosDrmFramebufferManager.cpp \
	display/ExynosDisplayFbInterface.cpp \
	display/ExynosDisplayInterface.cpp \
	display/ExynosLayer.cpp \
	primarydisplay/ExynosPrimaryDisplay.cpp \
	primarydisplay/ExynosPrimaryDisplayFbInterface.cpp \
	externaldisplay/ExynosExternalDisplay.cpp \
	externaldisplay/ExynosExternalDisplayFbInterface.cpp \
	virtualdisplay/ExynosVirtualDisplay.cpp \
	virtualdisplay/ExynosVirtualDisplayFbInterface.cpp \
	resources/ExynosMPP.cpp \
	utils/ExynosFenceTracer.cpp \
	utils/ExynosHWCDebug.cpp \
	utils/ExynosHWCFormat.cpp \
	utils/ExynosHWCHelper.cpp \
	utils/OneShotTimer.cpp

LOCAL_EXPORT_SHARED_LIBRARY_HEADERS += libacryl libdrm
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_C_INCLUDES)

include $(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/Android.mk

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-display\"
LOCAL_CFLAGS += -Wno-unused-parameter
ifeq ($(TARGET_BUILD_VARIANT), user)
LOCAL_CFLAGS += -Wno-unused-variable
endif
LOCAL_CFLAGS += -Wthread-safety

LOCAL_MODULE := libexynosdisplay
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/samsung_slsi-linaro/graphics/base/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)

################################################################################

ifeq ($(BOARD_USES_HWC_SERVICES),true)

include $(CLEAR_VARS)

LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers libexynos_headers
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libbinder libexynosdisplay libacryl \
	libui libion
LOCAL_STATIC_LIBRARIES += libVendorVideoApi
LOCAL_PROPRIETARY_MODULE := true

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/driver_header \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1 \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/libhwcService \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libdrmresource

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-service\"
LOCAL_CFLAGS += -Wthread-safety

LOCAL_SRC_FILES := \
	libhwcService/IExynosHWC.cpp \
	libhwcService/ExynosHWCService.cpp

LOCAL_HEADER_LIBRARIES += libhdrinterface_header libhdr10p_meta_interface_header
ifdef BOARD_LIBHDR_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR_PLUGIN)
endif
ifdef BOARD_LIBHDR10P_META_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR10P_META_PLUGIN)
endif

ifeq ($(BOARD_USES_DQE_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdqeInterface
LOCAL_HEADER_LIBRARIES += libdqeInterface_headers
endif

ifeq ($(BOARD_USES_DISPLAY_COLOR_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdisplaycolor_default
LOCAL_HEADER_LIBRARIES += libdisplaycolor_interface
endif

LOCAL_MODULE := libExynosHWCService
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/samsung_slsi-linaro/graphics/base/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)

endif

################################################################################

include $(CLEAR_VARS)

LOCAL_PRELINK_MODULE := false
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libexynosdisplay libacryl \
	libui libion
LOCAL_PROPRIETARY_MODULE := true
LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers libexynos_headers

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-2\"
LOCAL_CFLAGS += -Wthread-safety

ifeq ($(BOARD_USES_HWC_SERVICES),true)
LOCAL_CFLAGS += -DUSES_HWC_SERVICES
LOCAL_SHARED_LIBRARIES += libExynosHWCService
endif
LOCAL_STATIC_LIBRARIES += libVendorVideoApi

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/driver_header \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1 \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/libhwcService \

LOCAL_HEADER_LIBRARIES += libhdrinterface_header libhdr10p_meta_interface_header
ifdef BOARD_LIBHDR_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR_PLUGIN)
endif
ifdef BOARD_LIBHDR10P_META_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR10P_META_PLUGIN)
endif

ifeq ($(BOARD_USES_DQE_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdqeInterface
LOCAL_HEADER_LIBRARIES += libdqeInterface_headers
endif

ifeq ($(BOARD_USES_DISPLAY_COLOR_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdisplaycolor_default
LOCAL_HEADER_LIBRARIES += libdisplaycolor_interface
endif

LOCAL_SRC_FILES := \
	ExynosHWC.cpp

ifeq ($(TARGET_SOC),$(TARGET_BOOTLOADER_BOARD_NAME))
LOCAL_MODULE := hwcomposer.$(TARGET_BOOTLOADER_BOARD_NAME)
else
LOCAL_MODULE := hwcomposer.$(TARGET_SOC)
endif
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/samsung_slsi-linaro/graphics/base/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)

################################################################################

include $(CLEAR_VARS)

LOCAL_PROPRIETARY_MODULE := true

LOCAL_SHARED_LIBRARIES := liblog libcutils libutils libexynosdisplay libacryl \
                          libui libion libdrmresource

LOCAL_PROPRIETARY_MODULE := true
LOCAL_HEADER_LIBRARIES := libhardware_legacy_headers libbinder_headers libexynos_headers
LOCAL_STATIC_LIBRARIES := libgtest libgmock

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -DLOG_TAG=\"hwc-2\"

LOCAL_C_INCLUDES += \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/unittest \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/driver_header \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1 \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/device \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/utils \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/display \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/resources \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/primarydisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/externaldisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/$(TARGET_SOC_BASE)/libhwc2.1/virtualdisplay \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libhwc2.1/libhwcService \
	$(TOP)/hardware/samsung_slsi-linaro/graphics/base/libdrmresource

LOCAL_HEADER_LIBRARIES += libhdrinterface_header libhdr10p_meta_interface_header
ifdef BOARD_LIBHDR_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR_PLUGIN)
endif
ifdef BOARD_LIBHDR10P_META_PLUGIN
    LOCAL_SHARED_LIBRARIES += $(BOARD_LIBHDR10P_META_PLUGIN)
endif

ifeq ($(BOARD_USES_DQE_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdqeInterface
LOCAL_HEADER_LIBRARIES += libdqeInterface_headers
endif

ifeq ($(BOARD_USES_DISPLAY_COLOR_INTERFACE), true)
LOCAL_SHARED_LIBRARIES += libdisplaycolor_default
LOCAL_HEADER_LIBRARIES += libdisplaycolor_interface
endif

LOCAL_SRC_FILES := \
	unittests/main.cpp \
    unittests/HwcUnitTest.cpp

LOCAL_CFLAGS += -Wno-unused-parameter
LOCAL_CFLAGS += -Wno-unused-variable
LOCAL_MODULE := hwcomposer_unittest

include $(TOP)/hardware/samsung_slsi-linaro/graphics/base/BoardConfigCFlags.mk
include $(BUILD_EXECUTABLE)
