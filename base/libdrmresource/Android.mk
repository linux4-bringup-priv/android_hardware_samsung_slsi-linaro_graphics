# Copyright (C) 2021 The Android Open Source Project
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

LOCAL_SHARED_LIBRARIES := libcutils libdrm liblog libutils libhardware

LOCAL_PROPRIETARY_MODULE := true

LOCAL_SRC_FILES := \
	worker.cpp \
	resourcemanager.cpp \
	drmdevice.cpp \
	drmconnector.cpp \
	drmcrtc.cpp \
	drmencoder.cpp \
	drmmode.cpp \
	drmplane.cpp \
	drmproperty.cpp \
	drmeventlistener.cpp \
	vsyncworker.cpp

LOCAL_CFLAGS := -DHLOG_CODE=0
LOCAL_CFLAGS += -Wno-unused-parameter
LOCAL_CFLAGS += -Wthread-safety
LOCAL_EXPORT_SHARED_LIBRARY_HEADERS := libdrm

LOCAL_MODULE := libdrmresource
LOCAL_MODULE_TAGS := optional

include $(TOP)/hardware/samsung_slsi-linaro/graphics/base/BoardConfigCFlags.mk
include $(BUILD_SHARED_LIBRARY)

