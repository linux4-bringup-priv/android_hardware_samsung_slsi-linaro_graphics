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
#include "ExynosHWCService.h"
#include "ExynosResourceManager.h"
#include "ExynosVirtualDisplayModule.h"
#include "ExynosVirtualDisplay.h"
#include "ExynosExternalDisplay.h"
#include <binder/Parcel.h>
#define HWC_SERVICE_DEBUG 0

namespace android {

ANDROID_SINGLETON_STATIC_INSTANCE(ExynosHWCService);

ExynosHWCService::ExynosHWCService() : mHWCService(NULL),
                                       mExynosDevice(NULL),
                                       bootFinishedCallback(NULL) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "ExynosHWCService Constructor is called");
}

ExynosHWCService::~ExynosHWCService() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "ExynosHWCService Destructor is called");
}

int ExynosHWCService::setWFDMode(unsigned int mode) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::mode=%d", __func__, mode);
    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            return virtualdisplay->setWFDMode(mode);
        }
    }
    return INVALID_OPERATION;
}

int ExynosHWCService::getWFDMode() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            return virtualdisplay->getWFDMode();
        }
    }
    return INVALID_OPERATION;
}

int ExynosHWCService::getWFDInfo(int32_t *state, int32_t *compositionType, int32_t *format,
                                 int64_t *usage, int32_t *width, int32_t *height) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            return virtualdisplay->getWFDInfo(state, compositionType, format,
                                              usage, width, height);
        }
    }
    return INVALID_OPERATION;
}

int ExynosHWCService::sendWFDCommand(int32_t cmd, int32_t ext1, int32_t ext2) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::cmd=%d, ext1=%d, ext2=%d", __func__, cmd, ext1, ext2);
    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            switch (cmd) {
            case SET_TARGET_DISPLAY_LUMINANCE:
                mExynosDevice->mResourceManager->setTargetDisplayLuminance(ext1, ext2);
                break;
            case SET_TARGET_DISPLAY_DEVICE:
                mExynosDevice->mResourceManager->setTargetDisplayDevice(ext1);
            }
            return virtualdisplay->sendWFDCommand(cmd, ext1, ext2);
        }
    }
    return INVALID_OPERATION;
}

int ExynosHWCService::setWFDOutputResolution(unsigned int width, unsigned int height) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::width=%d, height=%d", __func__, width, height);

    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            return virtualdisplay->setWFDOutputResolution(width, height);
        }
    }
    return INVALID_OPERATION;
}

int ExynosHWCService::setVDSGlesFormat(int format) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::format=%d", __func__, format);

    for (uint32_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
        if (mExynosDevice->mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) {
            ExynosVirtualDisplay *virtualdisplay =
                (ExynosVirtualDisplay *)mExynosDevice->mDisplays[i];
            return virtualdisplay->setVDSGlesFormat(format);
        }
    }

    return INVALID_OPERATION;
}

int ExynosHWCService::getExternalHdrCapabilities() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);

    ExynosExternalDisplay *external_display =
        (ExynosExternalDisplay *)mExynosDevice->getDisplay(getDisplayId(HWC_DISPLAY_EXTERNAL, 0));

    if (external_display != nullptr)
        return external_display->mSinkHdrSupported;
    return 0;
}

void ExynosHWCService::setBootFinishedCallback(void (*callback)(ExynosDevice *)) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s, callback %p", __func__, callback);
    bootFinishedCallback = callback;
}

void ExynosHWCService::setBootFinished() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    if (bootFinishedCallback != NULL)
        bootFinishedCallback(mExynosDevice);
}

void ExynosHWCService::enableMPP(uint32_t physicalType, uint32_t physicalIndex, uint32_t logicalIndex, uint32_t enable) {
    ALOGD("%s:: type(%d), index(%d, %d), enable(%d)",
          __func__, physicalType, physicalIndex, logicalIndex, enable);
    mExynosDevice->enableMPP(physicalType, physicalIndex, logicalIndex, enable);
    mExynosDevice->invalidate();
}

void ExynosHWCService::setDumpCount(uint32_t dumpCount) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s, dmpCount(%d)", __func__, dumpCount);
    mExynosDevice->mTotalDumpCount = dumpCount;
    mExynosDevice->mIsDumpRequest = true;
    mExynosDevice->invalidate();
    return;
}

void ExynosHWCService::setHWCDebug(int debug) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s, debug %d", __func__, debug);
    mExynosDevice->setHWCDebug(debug);
}

uint32_t ExynosHWCService::getHWCDebug() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    return mExynosDevice->getHWCDebug();
}

void ExynosHWCService::setHWCFenceDebug(uint32_t fenceNum, uint32_t ipNum, uint32_t mode) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    mExynosDevice->setHWCFenceDebug(fenceNum, ipNum, mode);
}

void ExynosHWCService::getHWCFenceDebug() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    mExynosDevice->getHWCFenceDebug();
}

int ExynosHWCService::setHWCCtl(uint32_t display, uint32_t ctrl, int32_t val) {
    int err = 0;
    switch (ctrl) {
    case HWC_CTL_FORCE_GPU:
    case HWC_CTL_WINDOW_UPDATE:
    case HWC_CTL_DYNAMIC_RECOMP:
    case HWC_CTL_FORCE_PANIC:
    case HWC_CTL_SKIP_STATIC:
    case HWC_CTL_SKIP_M2M_PROCESSING:
    case HWC_CTL_SKIP_RESOURCE_ASSIGN:
    case HWC_CTL_SKIP_VALIDATE:
    case HWC_CTL_DUMP_MID_BUF:
    case HWC_CTL_CAPTURE_READBACK:
    case HWC_CTL_ENABLE_EXYNOSCOMPOSITION_OPT:
    case HWC_CTL_USE_MAX_G2D_SRC:
    case HWC_CTL_ENABLE_EARLY_START_MPP:
    case HWC_CTL_DISPLAY_MODE:
    case HWC_CTL_ENABLE_FENCE_TRACER:
    case HWC_CTL_SYS_FENCE_LOGGING:
    case HWC_CTL_DO_FENCE_FILE_DUMP:
    case HWC_CTL_USE_PERF_FILE:
    case HWC_CTL_ADJUST_DYNAMIC_RECOMP_TIMER:
        ALOGI("%s::%d on/off=%d", __func__, ctrl, val);
        mExynosDevice->setHWCControl(display, ctrl, val);
        break;
    default:
        ALOGE("%s: unsupported HWC_CTL, (%d)", __func__, ctrl);
        err = -1;
        break;
    }
    return err;
}

void ExynosHWCService::setInterfaceDebug(int32_t display, int32_t interface, int32_t value) {
    ALOGD("%s, display %d interface %d value %d", __func__, display, interface, value);

    switch (interface) {
    case HDR_INTERFACE_DEBUG:
    {
        ExynosDisplay *exynosDisplay = mExynosDevice->getDisplay(display);
        if (exynosDisplay == NULL) {
            for (size_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
                if (mExynosDevice->mDisplays[i]->mHdrCoefInterface)
                    mExynosDevice->mDisplays[i]->mHdrCoefInterface->setLogLevel(value);
            }
        } else if (exynosDisplay && exynosDisplay->mHdrCoefInterface) {
            exynosDisplay->mHdrCoefInterface->setLogLevel(value);
        }
    }
    break;
    case DQE_INTERFACE_DEBUG:
#ifdef USE_DQE_INTERFACE
    {
        if (mExynosDevice->mDqeCoefInterface)
            mExynosDevice->mDqeCoefInterface->setLogLevel(value);
        else
            ALOGD("%s, there is no DqeInterface", __func__);
    }
#endif
    break;
    case DISPLAY_COLOR_INTERFACE_DEBUG:
#ifdef USE_DISPLAY_COLOR_INTERFACE
    {
        ExynosDisplay *exynosDisplay = mExynosDevice->getDisplay(display);
        if (exynosDisplay == NULL) {
            for (size_t i = 0; i < mExynosDevice->mDisplays.size(); i++) {
                if (mExynosDevice->mDisplays[i]->mDisplayColorInterface)
                    mExynosDevice->mDisplays[i]->mDisplayColorInterface->setLogLevel(value);
            }
        } else if (exynosDisplay && exynosDisplay->mDisplayColorInterface) {
            exynosDisplay->mDisplayColorInterface->setLogLevel(value);
        }
    }
#endif
    break;
    default:
        ALOGI("invalid interface");
        break;
    }
}

int ExynosHWCService::printMppsAttr() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s", __func__);
    int res = mExynosDevice->printMppsAttr();

    return res;
}

int ExynosHWCService::getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *min_clock) {
    return mExynosDevice->getCPUPerfInfo(display, config, cpuIDs, min_clock);
}

int32_t ExynosHWCService::setDisplayMultiThreadedPresent(const int32_t& displayId,
                                                         const bool& enable) {
    auto display = mHWCCtx->device->getDisplay(displayId);

    if (display == nullptr) return -EINVAL;

    display->mDisplayControl.multiThreadedPresent = enable;
    ALOGD("ExynosHWCService::%s() display(%u) enable=%d", __func__, displayId, enable);
    return NO_ERROR;
}

int ExynosHWCService::createServiceLocked() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::", __func__);
    sp<IServiceManager> sm = defaultServiceManager();
    sm->addService(String16("Exynos.HWCService"), mHWCService, false);
    if (sm->checkService(String16("Exynos.HWCService")) != NULL) {
        ALOGD_IF(HWC_SERVICE_DEBUG, "adding Exynos.HWCService succeeded");
        return 0;
    } else {
        ALOGE_IF(HWC_SERVICE_DEBUG, "adding Exynos.HWCService failed");
        return -1;
    }
}

ExynosHWCService *ExynosHWCService::getExynosHWCService() {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s::", __func__);
    ExynosHWCService &instance = ExynosHWCService::getInstance();
    Mutex::Autolock _l(instance.mLock);
    if (instance.mHWCService == NULL) {
        instance.mHWCService = &instance;
        int status = ExynosHWCService::getInstance().createServiceLocked();
        if (status != 0) {
            ALOGE_IF(HWC_SERVICE_DEBUG, "getExynosHWCService failed");
        }
    }
    return instance.mHWCService;
}

void ExynosHWCService::setExynosDevice(ExynosDevice *device) {
    ALOGD_IF(HWC_SERVICE_DEBUG, "%s, device=%p", __func__, device);
    if (device) {
        mExynosDevice = device;
    }
}

status_t ExynosHWCService::onTransact(
    uint32_t code, const Parcel &data, Parcel *reply, uint32_t flags) {
    switch (code) {
    case SET_WFD_MODE: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int mode = data.readInt32();
        int res = setWFDMode(mode);
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case GET_WFD_MODE: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int res = getWFDMode();
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case GET_WFD_INFO: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int32_t state, compositionType, format, width, height;
        int64_t usage;
        int ret = getWFDInfo(&state, &compositionType, &format,
                             &usage, &width, &height);
        reply->writeInt32(state);
        reply->writeInt32(compositionType);
        reply->writeInt32(format);
        reply->writeInt64(usage);
        reply->writeInt32(width);
        reply->writeInt32(height);
        return ret;
    } break;
    case SEND_WFD_COMMAND: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int cmd = data.readInt32();
        int ext1 = data.readInt32();
        int ext2 = data.readInt32();
        int res = sendWFDCommand(cmd, ext1, ext2);
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case SET_WFD_OUTPUT_RESOLUTION: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int width = data.readInt32();
        int height = data.readInt32();
        int res = setWFDOutputResolution(width, height);
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case SET_VDS_GLES_FORMAT: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int format = data.readInt32();
        int res = setVDSGlesFormat(format);
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case HWC_CONTROL: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int display = data.readInt32();
        int ctrl = data.readInt32();
        int value = data.readInt32();
        int res = setHWCCtl(display, ctrl, value);
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case GET_EXTERNAL_HDR_CAPA: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int res = getExternalHdrCapabilities();
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case SET_BOOT_FINISHED: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        setBootFinished();
        return NO_ERROR;
    } break;
    case ENABLE_MPP: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        uint32_t type = data.readInt32();
        uint32_t physicalIdx = data.readInt32();
        uint32_t logicalIdx = data.readInt32();
        uint32_t enable = data.readInt32();
        enableMPP(type, physicalIdx, logicalIdx, enable);
        return NO_ERROR;
    } break;
    case SET_HWC_DEBUG: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int debug = data.readInt32();
        setHWCDebug(debug);
        reply->writeInt32(debug);
        return NO_ERROR;
    } break;
    case GET_HWC_DEBUG: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int debugFlag = getHWCDebug();
        reply->writeInt32(debugFlag);
        return NO_ERROR;
    } break;
    case SET_HWC_FENCE_DEBUG: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        uint32_t fenceNum = data.readInt32();
        uint32_t ipNum = data.readInt32();
        uint32_t mode = data.readInt32();
        setHWCFenceDebug(fenceNum, ipNum, mode);
        return NO_ERROR;
    } break;
    case GET_HWC_FENCE_DEBUG: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        getHWCFenceDebug();
        return NO_ERROR;
    } break;
    case GET_DUMP_LAYER: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        uint32_t dumpCount = data.readInt32();
        setDumpCount(dumpCount);
        return NO_ERROR;
    } break;
    case GET_CPU_PERF_INFO: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int32_t cpuIDs, min_clock;
        int display = data.readInt32();
        int config = data.readInt32();
        int ret = getCPUPerfInfo(display, config, &cpuIDs, &min_clock);
        reply->writeInt32(cpuIDs);
        reply->writeInt32(min_clock);
        return ret;
    } break;
    case SET_INTERFACE_DEBUG: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int32_t display = data.readInt32();
        int32_t interface = data.readInt32();
        int32_t value = data.readInt32();
        setInterfaceDebug(display, interface, value);
        return NO_ERROR;
    } break;
    case PRINT_MPP_ATTR: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int res = printMppsAttr();
        reply->writeInt32(res);
        return NO_ERROR;
    } break;
    case SET_DISPLAY_MULTI_THREADED_PRESENT: {
        CHECK_INTERFACE(IExynosHWCService, data, reply);
        int32_t displayId = data.readInt32();
        bool enable = data.readBool();
        int32_t error = setDisplayMultiThreadedPresent(displayId, enable);
        reply->writeInt32(error);
        return NO_ERROR;
    } break;
    default:
        return BBinder::onTransact(code, data, reply, flags);
    }
}
}  //namespace android
