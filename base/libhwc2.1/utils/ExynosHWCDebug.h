/*
 * Copyright (C) 2012 The Android Open Source Project
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
#ifndef HWC_DEBUG_H
#define HWC_DEBUG_H

#include <utils/String8.h>
#include "ExynosHWC.h"
#include "ExynosHWCHelper.h"

#ifdef BUILD_VARIANT_USER
#define DISABLE_HWC_DEBUG
#endif

enum {
    eDebugDefault = 0x00000001,
    eDebugWindowUpdate = 0x00000002,
    eDebugWinConfig = 0x00000004,
    eDebugSkipStaicLayer = 0x00000008,
    eDebugOverlaySupported = 0x00000010,
    eDebugResourceAssigning = 0x00000020,
    eDebugFence = 0x00000040,
    eDebugResourceManager = 0x00000080,
    eDebugMPP = 0x00000100,
    eDebugHWC = 0x00000200,
    eDebugLayer = 0x00000400,
    eDebugBuf = 0x00000800,
    eDebugVirtualDisplay = 0x00001000,
    eDebugCapacity = 0x00002000,
    eDebugExternalDisplay = 0x00004000,
    eDebugSkipResourceAssign = 0x00008000,
    eDebugSkipValidate = 0x00010000,
    eDebugDynamicRecomp = 0x00020000,
    eDebugDisplayInterfaceConfig = 0x00040000,
    eDebugResourceSetReserve = 0x00080000,
    eDebugAttrSetting = 0x00100000,
    eDebugColorManagement = 0x00200000,
    eDebugDisplayConfig = 0x00400000,
    eDebugTDM = 0x00800000,
    eDebugLoadBalancing = 0x01000000,
};

extern int hwcDebug;
#if defined(DISABLE_HWC_DEBUG)
inline bool hwcCheckDebugMessages(uint32_t __unused type) {
    return false;
}
#else
inline bool hwcCheckDebugMessages(uint32_t type) {
    return hwcDebug & type;
}
#endif

int32_t saveErrorLog(const android::String8 &errString, DisplayIdentifier &display);

#if defined(DISABLE_HWC_DEBUG)
#define HDEBUGLOGD(...)
#else
#define HDEBUGLOGD(type, ...)            \
    {                                    \
        if (hwcCheckDebugMessages(type)) \
            ALOGD(__VA_ARGS__);          \
    }
#endif
#define HDEBUGLOGV(type, ...) \
    ALOGV(__VA_ARGS__);
#define HDEBUGLOGE(type, ...) \
    ALOGE(__VA_ARGS__);

#if defined(DISABLE_HWC_DEBUG)
#define DISPLAY_LOGD(...)
#define MPP_LOGD(...)
#else
#define DISPLAY_LOGD(type, msg, ...)                                                 \
    {                                                                                \
        if (hwcCheckDebugMessages(type))                                             \
            ALOGD("%s:: [%s] " msg, __func__, mDisplayName.c_str(), ##__VA_ARGS__); \
    }
#define MPP_LOGD(type, msg, ...)                                                                 \
    {                                                                                            \
        if (hwcCheckDebugMessages(type))                                                         \
            ALOGD("%s:: [%s][%d] " msg, __func__, mName.c_str(), mLogicalIndex, ##__VA_ARGS__); \
    }
#endif
#define DISPLAY_LOGV(msg, ...) ALOGV("[%s] " msg, mDisplayName.c_str(), ##__VA_ARGS__)
#define DISPLAY_LOGI(msg, ...) ALOGI("[%s] " msg, mDisplayName.c_str(), ##__VA_ARGS__)
#define DISPLAY_LOGW(msg, ...) ALOGW("[%s] " msg, mDisplayName.c_str(), ##__VA_ARGS__)
#define DISPLAY_LOGE(msg, ...)                                          \
    {                                                                   \
        ALOGE("[%s] " msg, mDisplayName.c_str(), ##__VA_ARGS__);       \
        String8 saveString;                                             \
        saveString.appendFormat(msg, ##__VA_ARGS__);                    \
        saveErrorLog(saveString, this->mDisplayInfo.displayIdentifier); \
    }

#define MPP_LOGV(msg, ...) ALOGV("[%s][%d] " msg, mName.c_str(), mLogicalIndex, ##__VA_ARGS__)
#define MPP_LOGI(msg, ...) ALOGI("[%s][%d] " msg, mName.c_str(), mLogicalIndex, ##__VA_ARGS__)
#define MPP_LOGW(msg, ...) ALOGW("[%s][%d] " msg, mName.c_str(), mLogicalIndex, ##__VA_ARGS__)
#define MPP_LOGE(msg, ...)                                                    \
    {                                                                         \
        ALOGE("[%s][%d] " msg, mName.c_str(), mLogicalIndex, ##__VA_ARGS__); \
        String8 saveString;                                                   \
        saveString.appendFormat(msg, ##__VA_ARGS__);                          \
        saveErrorLog(saveString, mAssignedDisplayInfo.displayIdentifier);     \
    }

#define HWC_LOGE(display, msg, ...)                  \
    {                                                \
        ALOGE(msg, ##__VA_ARGS__);                   \
        String8 saveString;                          \
        saveString.appendFormat(msg, ##__VA_ARGS__); \
        saveErrorLog(saveString, display);           \
    }
#define HWC_LOGE_NODISP(msg, ...)                    \
    {                                                \
        ALOGE(msg, ##__VA_ARGS__);                   \
        String8 saveString;                          \
        saveString.appendFormat(msg, ##__VA_ARGS__); \
        DisplayIdentifier empty;                     \
        saveErrorLog(saveString, empty);             \
    }
#endif
