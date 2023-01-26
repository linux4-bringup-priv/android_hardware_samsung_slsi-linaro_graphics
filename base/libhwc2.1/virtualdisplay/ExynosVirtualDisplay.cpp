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

#undef LOG_TAG
#define LOG_TAG "hwc-virt-display"
#include "ExynosVirtualDisplay.h"
#include "ExynosLayer.h"

#include "ExynosHWCHelper.h"
#include "ExynosGraphicBuffer.h"

#include <cutils/properties.h>

using vendor::graphics::BufferUsage;
using vendor::graphics::ExynosGraphicBufferUsage;

#define SKIP_FRAME_COUNT_FOR_FB_INTERFACE 5
#define SKIP_FRAME_COUNT_FOR_DRM_INTERFACE 1

extern struct exynos_hwc_control exynosHWCControl;

ExynosVirtualDisplay::ExynosVirtualDisplay(DisplayIdentifier node)
    : ExynosDisplay(node) {
    mDisplayControl.earlyStartMPP = false;

    mOutputBufferAcquireFenceFd = -1;
    mOutputBufferReleaseFenceFd = -1;

    mIsWFDState = 0;
    mIsSecureVDSState = false;
    mIsSkipFrame = false;
    mPresentationMode = false;

    mSkipFrameCount = -1;

    // TODO : Hard coded currently
    mNumMaxPriorityAllowed = 1;

    mDisplayWidth = 0;
    mDisplayHeight = 0;
    mOutputBuffer = NULL;
    mCompositionType = COMPOSITION_GLES;
    mGLESFormat = HAL_PIXEL_FORMAT_RGBA_8888;
    mSinkUsage = BufferUsage::COMPOSER_OVERLAY | BufferUsage::VIDEO_ENCODER;
    mIsSecureDRM = false;
    mIsNormalDRM = false;
    mNeedReloadResourceForHWFC = false;
    mMinTargetLuminance = 0;
    mMaxTargetLuminance = 100;
    mSinkDeviceType = 0;

    mUseDpu = false;
    mDisplayControl.enableExynosCompositionOptimization = false;
    mIsFirstFrameDisplayed = false;
    mExternalPlugState = false;
}

ExynosVirtualDisplay::~ExynosVirtualDisplay() {
}

void ExynosVirtualDisplay::createVirtualDisplay(uint32_t width, uint32_t height, int32_t *format) {
    DISPLAY_LOGI("%s:: width(%d), height(%d), format(%d)", __func__, width, height, *format);

    initDisplay();

    // Virtual Display don't use skip static layer.
    mClientCompositionInfo.mEnableSkipStatic = false;

    mPlugState = true;
    mDisplayWidth = width;
    mDisplayHeight = height;
    mXres = width;
    mYres = height;
    mGLESFormat = *format;

    if (mUseDpu) {
        mDisplayInterface->setPowerMode(HWC_POWER_MODE_NORMAL);
        if (mDisplayInterface->mType == INTERFACE_TYPE_FB)
            mSkipFrameCount = SKIP_FRAME_COUNT_FOR_FB_INTERFACE;
        else
            mSkipFrameCount = SKIP_FRAME_COUNT_FOR_DRM_INTERFACE;
        mIsFirstFrameDisplayed = false;
        mDisplayInterface->setRepeaterBuffer(true);
    }
    mPowerModeState = HWC2_POWER_MODE_ON;
}

void ExynosVirtualDisplay::destroyVirtualDisplay() {
    DISPLAY_LOGI("%s", __func__);

    if (mUseDpu && mPlugState) {
        mDisplayInterface->setRepeaterBuffer(false);
        if (mDisplayInterface->mType == INTERFACE_TYPE_DRM) {
            waitPreviousFrameDone(mLastPresentFence);
            mDisplayInterface->clearDisplay();
        }
        mDisplayInterface->setPowerMode(HWC_POWER_MODE_OFF);
    }

    mPlugState = false;
    mDisplayWidth = 0;
    mDisplayHeight = 0;
    mXres = 0;
    mYres = 0;
    mMinTargetLuminance = 0;
    mMaxTargetLuminance = 100;
    mSinkDeviceType = 0;
    mCompositionType = COMPOSITION_HWC;
    mGLESFormat = HAL_PIXEL_FORMAT_RGBA_8888;

    mNeedReloadResourceForHWFC = false;

    mDisplayInterface->onDisplayRemoved();
    mPowerModeState = HWC2_POWER_MODE_OFF;
}

int ExynosVirtualDisplay::setWFDMode(unsigned int mode) {
    DISPLAY_LOGI("%s:: mode(%u)", __func__, mode);

    property_set("vendor.wfd.mode", mode ? "1" : "0");

    if ((mode == GOOGLEWFD_TO_LLWFD || mode == LLWFD_TO_GOOGLEWFD))
        mNeedReloadResourceForHWFC = true;
    mIsWFDState = mode;
    return HWC2_ERROR_NONE;
}

int ExynosVirtualDisplay::getWFDMode() {
    DISPLAY_LOGI("%s:: mode(%d)", __func__, mIsWFDState);
    return mIsWFDState;
}

int ExynosVirtualDisplay::getWFDInfo(int32_t *state, int32_t *compositionType, int32_t *format,
                                     int64_t *usage, int32_t *width, int32_t *height) {
    *state = mIsWFDState;
    *compositionType = mCompositionType;
    if (mIsSkipFrame || !mPlugState)
        *format = (int32_t)0xFFFFFFFF;
    else if (mIsSecureDRM && !mIsSecureVDSState)
        *format = (int32_t)HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN;
    else
        *format = (int32_t)mGLESFormat;
    *usage = mSinkUsage;
    *width = mDisplayWidth;
    *height = mDisplayHeight;

    return HWC2_ERROR_NONE;
}

int ExynosVirtualDisplay::sendWFDCommand(int32_t cmd, int32_t ext1, int32_t ext2) {
    DISPLAY_LOGI("%s:: cmd(%d), ext1(%d), ext2(%d)", __func__, cmd, ext1, ext2);

    int ret = 0;

    switch (cmd) {
    case SET_WFD_MODE:
        /* ext1: mode, ext2: unused */
        ret = setWFDMode(ext1);
        break;
    case SET_TARGET_DISPLAY_LUMINANCE:
        /* ext1: min, ext2: max */
        if (mHdrCoefInterface) {
            mHdrTargetInfo.min_luminance = (unsigned int)ext1;
            mHdrTargetInfo.max_luminance = (unsigned int)ext2;
            mHdrCoefInterface->setTargetInfo(&mHdrTargetInfo);
        }
        mMinTargetLuminance = (uint16_t)ext1;
        mMaxTargetLuminance = (uint16_t)ext2;
        break;
    case SET_TARGET_DISPLAY_DEVICE:
        /* ext1: type, ext2: unused */
        mSinkDeviceType = ext1;
        break;
    default:
        DISPLAY_LOGE("invalid cmd(%d)", cmd);
        break;
    }

    return ret;
}

int ExynosVirtualDisplay::setSecureVDSMode(unsigned int mode) {
    DISPLAY_LOGI("%s:: mode(%u)", __func__, mode);
    mIsWFDState = mode;
    mIsSecureVDSState = !!mode;
    return HWC2_ERROR_NONE;
}

int ExynosVirtualDisplay::setWFDOutputResolution(
    unsigned int width, unsigned int height) {
    DISPLAY_LOGI("%s:: width(%u), height(%u)", __func__, width, height);
    mDisplayWidth = width;
    mDisplayHeight = height;
    mXres = width;
    mYres = height;
    return HWC2_ERROR_NONE;
}

void ExynosVirtualDisplay::getWFDOutputResolution(
    unsigned int *width, unsigned int *height) {
    *width = mDisplayWidth;
    *height = mDisplayHeight;
}

void ExynosVirtualDisplay::setPresentationMode(bool use) {
    mPresentationMode = use;
}

int ExynosVirtualDisplay::getPresentationMode(void) {
    return mPresentationMode;
}

int ExynosVirtualDisplay::setVDSGlesFormat(int format) {
    DISPLAY_LOGI("%s:: 0x%x", __func__, format);
    mGLESFormat = format;
    return HWC2_ERROR_NONE;
}

bool ExynosVirtualDisplay::is2StepBlendingRequired(exynos_image &src, buffer_handle_t outbuf) {
    return false;
}

int32_t ExynosVirtualDisplay::setOutputBuffer(
    buffer_handle_t buffer, int32_t releaseFence) {
    mOutputBuffer = buffer;
    if (mOutputBufferReleaseFenceFd >= 0) {
        mOutputBufferReleaseFenceFd = mFenceTracer.fence_close(
            mOutputBufferReleaseFenceFd, mDisplayInfo.displayIdentifier,
            FENCE_TYPE_DST_RELEASE, FENCE_IP_OUTBUF,
            "ExynosVirtualDisplay::setOutputBuffer: standalone writeback rel_fence");
    }

    mOutputBufferReleaseFenceFd = mFenceTracer.hwc_dup(releaseFence, mDisplayInfo.displayIdentifier,
                                                       FENCE_TYPE_DST_RELEASE, FENCE_IP_OUTBUF, true);

    if (!mUseDpu && mExynosCompositionInfo.mM2mMPP != NULL) {
        mExynosCompositionInfo.mM2mMPP->setOutBuf(mOutputBuffer,
                                                  mOutputBufferReleaseFenceFd, mDisplayInfo);
        mOutputBufferReleaseFenceFd = -1;
    }

    DISPLAY_LOGD(eDebugVirtualDisplay, "setOutputBuffer(), mOutputBufferReleaseFenceFd %d", mOutputBufferReleaseFenceFd);
    return HWC2_ERROR_NONE;
}

int ExynosVirtualDisplay::clearDisplay() {
    return 0;
}

void ExynosVirtualDisplay::doPreProcessing(
    DeviceValidateInfo &validateInfo,
    uint64_t &geometryChanged) {
    ExynosDisplay::doPreProcessing(validateInfo, geometryChanged);

    if (mUseDpu)
        return;

    /*
     * If there is layer that has priority higher than ePriorityMid
     * exynos composition handles only one layer that has the highest priority.
     * If there are more than one layer with same priority
     * exynos composition handles top layer.
     */
    int32_t maxPriorityIndex = -1;
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mOverlayPriority >= ePriorityHigh) {
            DISPLAY_LOGD(eDebugResourceManager, "\t[%zu] layer has high priority(%d)",
                         i, mLayers[i]->mOverlayPriority);
            if (maxPriorityIndex < 0) {
                maxPriorityIndex = i;
            } else {
                if (mLayers[i]->mOverlayPriority >= mLayers[maxPriorityIndex]->mOverlayPriority)
                    maxPriorityIndex = i;
            }
        }
    }
    if (maxPriorityIndex >= 0) {
        DISPLAY_LOGD(eDebugResourceManager, "\texynos composition will be assgined for only [%d] layer", maxPriorityIndex);
    }

    for (size_t i = 0; i < mLayers.size(); i++) {
        if ((maxPriorityIndex >= 0) &&
            (i != maxPriorityIndex)) {
            mLayers[i]->mLayerFlag |= EXYNOS_HWC_FORCE_CLIENT_WFD;
        } else {
            mLayers[i]->mLayerFlag &= ~(EXYNOS_HWC_FORCE_CLIENT_WFD);
        }
    }
}

int32_t ExynosVirtualDisplay::preProcessValidate(
    DeviceValidateInfo &validateInfo,
    uint64_t &geometryChanged) {
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s", __func__);
    initPerFrameData();

    mClientCompositionInfo.setCompressionType(COMP_TYPE_NONE);

    if (mSkipFrameCount > 0)
        setGeometryChanged(GEOMETRY_DISPLAY_FRAME_SKIPPED, geometryChanged);

    return ExynosDisplay::preProcessValidate(validateInfo, geometryChanged);
}

int32_t ExynosVirtualDisplay::postProcessValidate() {
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s", __func__);
    int32_t ret = ExynosDisplay::postProcessValidate();

    if (checkSkipFrame()) {
        handleSkipFrame();
        if (mSkipFrameCount > 0)
            mSkipFrameCount--;
    } else {
        setDrmMode();
        setSinkBufferUsage();
        setCompositionType();
        mIsFirstFrameDisplayed = true;
    }

    return ret;
}

int32_t ExynosVirtualDisplay::canSkipValidate() {
    if (checkSkipFrame() || mIsFirstFrameDisplayed == false)
        return SKIP_ERR_FORCE_VALIDATE;

    return ExynosDisplay::canSkipValidate();
}

int32_t ExynosVirtualDisplay::presentDisplay(
    DevicePresentInfo &presentInfo, int32_t *outPresentFence) {
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mCompositionType(%d)", __func__,
                 mCompositionType);

    int32_t ret = HWC2_ERROR_NONE;

    if (mIsSkipFrame) {
        handleAcquireFence();
        initPerFrameData();
        return ret;
    }

    ret = ExynosDisplay::presentDisplay(presentInfo, outPresentFence);

    if (!mUseDpu && *outPresentFence == -1 && mOutputBufferAcquireFenceFd >= 0) {
        *outPresentFence = mOutputBufferAcquireFenceFd;
        mOutputBufferAcquireFenceFd = -1;
    }

    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: outPresentFence(%d)", __func__, *outPresentFence);

    return ret;
}

int ExynosVirtualDisplay::setWinConfigData(DevicePresentInfo &presentInfo) {
    int ret = NO_ERROR;

    if (mUseDpu) {
        buffer_handle_t outbufHandle = mOutputBuffer;

        ret = ExynosDisplay::setWinConfigData(presentInfo);

        if (outbufHandle && !(mSkipFrameCount > 0)) {
            mDpuData.enable_standalone_writeback = true;
            mDpuData.standalone_writeback_info.handle = outbufHandle;
            mDpuData.standalone_writeback_info.rel_fence =
                mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                             FENCE_TYPE_DST_RELEASE, FENCE_IP_OUTBUF, mOutputBufferReleaseFenceFd);
            mOutputBufferReleaseFenceFd = -1;
        } else {
            DISPLAY_LOGE("setWinConfigData() should be skipped");
            ret = -EINVAL;
        }
    }

    return ret;
}

int ExynosVirtualDisplay::setDisplayWinConfigData() {
    return NO_ERROR;
}

int32_t ExynosVirtualDisplay::validateWinConfigData() {
    int ret = NO_ERROR;

    if (mUseDpu) {
        ret = ExynosDisplay::validateWinConfigData();
    }

    return ret;
}

int ExynosVirtualDisplay::deliverWinConfigData(DevicePresentInfo &presentInfo) {
    int ret = NO_ERROR;

    if (mUseDpu) {
        ret = ExynosDisplay::deliverWinConfigData(presentInfo);
    }

    return 0;
}

int ExynosVirtualDisplay::setReleaseFences() {
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mOutputBufferAcquireFenceFd(%d)", __func__, mOutputBufferAcquireFenceFd);

    int ret = 0;

    if (!mUseDpu && mClientCompositionInfo.mHasCompositionLayer) {
        int fence;
        uint32_t framebufferTargetIndex;
        framebufferTargetIndex = mExynosCompositionInfo.mM2mMPP->getAssignedSourceNum() - 1;
        fence = mExynosCompositionInfo.mM2mMPP->getSrcReleaseFence(framebufferTargetIndex);
        if (fence > 0)
            mFenceTracer.fence_close(fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB);
    }

    ret = ExynosDisplay::setReleaseFences();

    if (!mUseDpu) {
        mOutputBufferAcquireFenceFd =
            mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_PRESENT, FENCE_IP_G2D, mExynosCompositionInfo.mAcquireFence);
        mFenceTracer.setFenceInfo(mExynosCompositionInfo.mAcquireFence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_PRESENT, FENCE_IP_G2D, FENCE_TO);
        mExynosCompositionInfo.mAcquireFence = -1;
    }

    /* mClientCompositionInfo.mAcquireFence is delivered to G2D or DPU */
    mClientCompositionInfo.mAcquireFence = -1;

    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mOutputBufferAcquireFenceFd(%d)", __func__, mOutputBufferAcquireFenceFd);
    return ret;
}

bool ExynosVirtualDisplay::checkFrameValidation() {
    if (mOutputBuffer == NULL) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mOutputBuffer is NULL", __func__);
        handleAcquireFence();
        return false;
    }

    buffer_handle_t outbufHandle = mOutputBuffer;
    if (outbufHandle == NULL) {
        DISPLAY_LOGE("%s:: dynamicCast is failed", __func__);
        handleAcquireFence();
        return false;
    }

    if (mCompositionType != COMPOSITION_HWC) {
        if (mClientCompositionInfo.mTargetBuffer == NULL) {
            DISPLAY_LOGE("%s:: Client target buffer is NULL", __func__);
            handleAcquireFence();
            return false;
        }
    }

    return true;
}

void ExynosVirtualDisplay::setSinkBufferUsage() {
    mSinkUsage = BufferUsage::COMPOSER_OVERLAY | BufferUsage::VIDEO_ENCODER;
    if (mIsSecureDRM) {
        mSinkUsage |= BufferUsage::CPU_READ_NEVER |
                      BufferUsage::CPU_WRITE_NEVER |
                      BufferUsage::PROTECTED;
    } else if (mIsNormalDRM)
        mSinkUsage |= ExynosGraphicBufferUsage::PRIVATE_NONSECURE;
}

void ExynosVirtualDisplay::setCompositionType() {
    size_t compositionClientLayerCount = 0;
    size_t CompositionDeviceLayerCount = 0;
    ;
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        if (layer->mValidateCompositionType == HWC2_COMPOSITION_CLIENT ||
            layer->mValidateCompositionType == HWC2_COMPOSITION_INVALID) {
            compositionClientLayerCount++;
        }
        if (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE ||
            layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS) {
            CompositionDeviceLayerCount++;
        }
    }
    if (compositionClientLayerCount > 0 && CompositionDeviceLayerCount > 0) {
        mCompositionType = COMPOSITION_MIXED;
    } else if (CompositionDeviceLayerCount > 0) {
        mCompositionType = COMPOSITION_HWC;
    } else {
        mCompositionType = COMPOSITION_GLES;
    }

    if (mCompositionType == COMPOSITION_GLES)
        mCompositionType = COMPOSITION_MIXED;

    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: compositionClientLayerCount(%zu), CompositionDeviceLayerCount(%zu), mCompositionType(%d)",
                 __func__, compositionClientLayerCount, CompositionDeviceLayerCount, mCompositionType);
}

void ExynosVirtualDisplay::initPerFrameData() {
    mIsSkipFrame = false;
    mIsSecureDRM = false;
    mIsNormalDRM = false;
    mCompositionType = COMPOSITION_HWC;
    mSinkUsage = BufferUsage::COMPOSER_OVERLAY | BufferUsage::VIDEO_ENCODER;
}

bool ExynosVirtualDisplay::checkSkipFrame() {
    if (mLayers.size() == 0) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: there is no layer", __func__);
        return true;
    }

    if (mSkipFrameCount > 0) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mSkipFrameCount(%d)", __func__, mSkipFrameCount);
        return true;
    }

    if (mIsWFDState == 0) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: this is not wfd scenario", __func__);
        return true;
    }

    if (mIsWFDState == GOOGLEWFD_TO_LLWFD || mIsWFDState == LLWFD_TO_GOOGLEWFD) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mIsWFDState(%d)", __func__, mIsWFDState);
        return true;
    }

    if (mExternalPlugState) {
        DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mExternalPlugState(%d)", __func__, mExternalPlugState);
        return true;
    }

    return false;
}

void ExynosVirtualDisplay::setDrmMode() {
    mIsSecureDRM = false;
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        if ((layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE ||
             layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS) &&
            layer->mLayerBuffer && getDrmMode(layer->mLayerBuffer) == SECURE_DRM) {
            mIsSecureDRM = true;
            DISPLAY_LOGD(eDebugVirtualDisplay, "include secure drm layer");
        }
        if ((layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE ||
             layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS) &&
            layer->mLayerBuffer && getDrmMode(layer->mLayerBuffer) == NORMAL_DRM) {
            mIsNormalDRM = true;
            DISPLAY_LOGD(eDebugVirtualDisplay, "include normal drm layer");
        }
    }
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s:: mIsSecureDRM(%d), mIsNormalDRM(%d)", __func__, mIsSecureDRM, mIsNormalDRM);
}

void ExynosVirtualDisplay::handleSkipFrame() {
    mIsSkipFrame = true;
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        if (mUseDpu)
            layer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
        else
            layer->mValidateCompositionType = HWC2_COMPOSITION_EXYNOS;
    }
    mIsSecureDRM = false;
    mIsNormalDRM = false;
    mCompositionType = COMPOSITION_HWC;
    mSinkUsage = BufferUsage::COMPOSER_OVERLAY | BufferUsage::VIDEO_ENCODER;
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s", __func__);
}

void ExynosVirtualDisplay::handleAcquireFence() {
    /* handle fence of DEVICE or EXYNOS composition layers */
    for (size_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->mAcquireFence = mFenceTracer.fence_close(layer->mAcquireFence,
                                                        mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL);
    }

    mClientCompositionInfo.mAcquireFence = mFenceTracer.fence_close(
        mClientCompositionInfo.mAcquireFence, mDisplayInfo.displayIdentifier,
        FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB);
    mOutputBufferReleaseFenceFd = mFenceTracer.fence_close(
        mOutputBufferReleaseFenceFd, mDisplayInfo.displayIdentifier,
        FENCE_TYPE_DST_RELEASE, FENCE_IP_ALL);
    DISPLAY_LOGD(eDebugVirtualDisplay, "%s", __func__);
}

int32_t ExynosVirtualDisplay::getHdrCapabilities(uint32_t *outNumTypes,
                                                 int32_t *outTypes, float *outMaxLuminance,
                                                 float *outMaxAverageLuminance, float *outMinLuminance) {
    if (outTypes == NULL) {
        *outNumTypes = 1;
        return HWC2_ERROR_NONE;
    }
    outTypes[0] = HAL_HDR_HDR10;
    return HWC2_ERROR_NONE;
}

void ExynosVirtualDisplay::initDisplayInterface(uint32_t interfaceType,
                                                void *deviceData, size_t &deviceDataSize) {
    ExynosDisplay::initDisplayInterface(interfaceType,
            deviceData, deviceDataSize);
}

void ExynosVirtualDisplay::init(uint32_t maxWindowNum, ExynosMPP *blendingMPP) {
    ExynosDisplay::init(maxWindowNum, blendingMPP);
    mHdrCoefInterface = createHdrInterfaceInstance();
    mHdr10PMetaInterface = createHdr10PMetaInterfaceInstance();
    if (mHdrCoefInterface) {
        mHdrCoefInterface->sethdr10pMetaInterface(mHdr10PMetaInterface);
        mHdrTargetInfo.bpc = HDR_BPC_8;
        mHdrTargetInfo.hdr_capa = HDR_CAPA_OUTER;
        mHdrTargetInfo.min_luminance = 0;
        mHdrTargetInfo.max_luminance = 100;
        mHdrTargetInfo.dataspace =
            (android_dataspace)(HAL_DATASPACE_STANDARD_BT709 |
                                HAL_DATASPACE_TRANSFER_GAMMA2_2 |
                                HAL_DATASPACE_RANGE_LIMITED);

        mHdrCoefInterface->setTargetInfo(&mHdrTargetInfo);
    }
}

bool ExynosVirtualDisplay::checkDisplayUnstarted() {
    return (mPlugState == true) && (mSkipFrameCount > 0);
}

void ExynosVirtualDisplay::setExternalPlugState(bool state,
                                                uint64_t &geometryChanged) {
    DISPLAY_LOGI("%s:: hotplug status of external display: %d", __func__, state);

    mExternalPlugState = state;
    if (mPlugState)
        setGeometryChanged(GEOMETRY_DISPLAY_FRAME_SKIPPED, geometryChanged);
}
