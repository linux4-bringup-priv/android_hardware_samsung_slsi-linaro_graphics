/* * Copyright (C) 2012 The Android Open Source Project
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

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
//#include <linux/fb.h>
#include <processgroup/processgroup.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <cutils/properties.h>
#include <utils/CallStack.h>
#include <hardware/hwcomposer_defs.h>
#include <android/sync.h>
#include <cmath>

#include <map>
#include "ExynosDisplay.h"
#include "ExynosExternalDisplay.h"
#include "ExynosLayer.h"
#include "exynos_format.h"
#include "ExynosFenceTracer.h"
#include "TraceUtils.h"

#include <sys/mman.h>
#include <unistd.h>

#include "ExynosGraphicBuffer.h"

/**
 * ExynosDisplay implementation
 */

using namespace android;
using namespace vendor::graphics;
using namespace std::chrono_literals;

extern uint64_t errorFrameCount;
extern struct exynos_hwc_control exynosHWCControl;
extern struct update_time_info updateTimeInfo;

constexpr auto microsecsPerSec = std::chrono::microseconds(1s).count();

int ExynosSortedLayer::compare(ExynosLayer *const *lhs, ExynosLayer *const *rhs) {
    ExynosLayer *left = *((ExynosLayer **)(lhs));
    ExynosLayer *right = *((ExynosLayer **)(rhs));
    return left->mZOrder > right->mZOrder;
}

ssize_t ExynosSortedLayer::remove(const ExynosLayer *item) {
    for (size_t i = 0; i < size(); i++) {
        if (array()[i] == item) {
            removeAt(i);
            return i;
        }
    }
    return -1;
}

status_t ExynosSortedLayer::vector_sort() {
    return sort(compare);
}

bool ExynosVsyncCallback::Callback(uint64_t timestamp) {
    /*
     * keep vsync period if mVsyncTimeStamp
     * is not initialized since vsync is enabled
     */
    if (mVsyncTimeStamp > 0) {
        mVsyncPeriod = timestamp - mVsyncTimeStamp;
    }

    mVsyncTimeStamp = timestamp;

    /* There was no config chage request */
    if (!mDesiredVsyncPeriod)
        return true;

    /*
     * mDesiredVsyncPeriod is nanoseconds
     * Compare with milliseconds
     */
    if (mDesiredVsyncPeriod / microsecsPerSec == mVsyncPeriod / microsecsPerSec)
        return true;

    return false;
}

ExynosCompositionInfo::ExynosCompositionInfo(uint32_t type)
    : ExynosMPPSource(MPP_SOURCE_COMPOSITION_TARGET, this),
      mType(type),
      mHasCompositionLayer(false),
      mFirstIndex(-1),
      mLastIndex(-1),
      mTargetBuffer(NULL),
      mDataSpace(HAL_DATASPACE_UNKNOWN),
      mAcquireFence(-1),
      mReleaseFence(-1),
      mEnableSkipStatic(false),
      mSkipStaticInitFlag(false),
      mSkipFlag(false),
      mWindowIndex(-1) {
    /* If AFBC compression of mTargetBuffer is changed, */
    /* mCompressionInfo should be set properly before resource assigning */

    char value[256];
    int afbc_prop;
    property_get("ro.vendor.ddk.set.afbc", value, "0");
    afbc_prop = atoi(value);

    if (afbc_prop == 0)
        mCompressionInfo.type = COMP_TYPE_NONE;
    else
        mCompressionInfo.type = COMP_TYPE_AFBC;

    mSkipSrcInfo.reset();

    if (type == COMPOSITION_CLIENT) {
        mEnableSkipStatic = true;
#ifdef USES_SAJC_FEATURE
        mCompressionInfo.type = COMP_TYPE_SAJC;
#endif
    } else {
        if (afbc_prop) {
            mFormat = ExynosMPP::defaultMppDstCompFormat;
        } else {
            mFormat = ExynosMPP::defaultMppDstFormat;
        }
    }

    mLastWinConfigData.reset();
}

void ExynosCompositionInfo::initializeInfos() {
    mHasCompositionLayer = false;
    mFirstIndex = -1;
    mLastIndex = -1;
    mTargetBuffer = NULL;
    if (mType == COMPOSITION_EXYNOS)
        mDataSpace = HAL_DATASPACE_UNKNOWN;

    if (mAcquireFence >= 0) {
        ALOGD("ExynosCompositionInfo(%d):: mAcquire is not initialized(%d)", mType, mAcquireFence);
        mFenceTracer.fence_close(mAcquireFence,
                                 mDisplayIdentifier, FENCE_TYPE_UNDEFINED, FENCE_IP_UNDEFINED,
                                 "display::initializeInfos: mAcquireFence");
    }
    mAcquireFence = -1;
    if (mReleaseFence >= 0) {
        ALOGD("ExynosCompositionInfo(%d):: mReleaseFence is not initialized(%d)", mType, mReleaseFence);
        mFenceTracer.fence_close(mReleaseFence,
                                 mDisplayIdentifier, FENCE_TYPE_UNDEFINED, FENCE_IP_UNDEFINED,
                                 "display::initializeInfos: mReleaseFence");
    }
    mReleaseFence = -1;
    mWindowIndex = -1;
    mOtfMPP = NULL;
    if ((mType == COMPOSITION_EXYNOS) &&
        (mM2mMPP == NULL)) {
        mM2mMPP = mBlendingMPP;
        if (mM2mMPP == NULL)
            ALOGV("display[%d]  m2mMPP for Blending is NULL",
                  mDisplayIdentifier.type);
    }
}

bool ExynosCompositionInfo::setTargetBuffer(
    buffer_handle_t handle, int32_t acquireFence,
    android_dataspace dataspace) {
    mTargetBuffer = handle;
    if (mType == COMPOSITION_CLIENT) {
        mAcquireFence = mFenceTracer.checkFenceDebug(mDisplayIdentifier,
                                                     FENCE_TYPE_DST_ACQUIRE, FENCE_IP_FB, acquireFence);
    } else {
        mAcquireFence = mFenceTracer.checkFenceDebug(mDisplayIdentifier,
                                                     FENCE_TYPE_DST_ACQUIRE, FENCE_IP_G2D, acquireFence);
    }
    mDataSpace = dataspace;
    ExynosGraphicBufferMeta gmeta(handle);

    BufInfo newBufInfo = BufInfo(gmeta.width, gmeta.height, gmeta.format,
                                 gmeta.producer_usage, gmeta.consumer_usage);
    bool bufChanged = (mBufInfo.isValid() && (mBufInfo != newBufInfo));
    mBufInfo = newBufInfo;
    return bufChanged;
}

void ExynosCompositionInfo::setCompressionType(uint32_t compressionType) {
    mCompressionInfo.type = compressionType;
}

void ExynosCompositionInfo::dump(String8 &result) {
    result.appendFormat("CompositionInfo (%d)\n", mType);
    result.appendFormat("mHasCompositionLayer(%d)\n", mHasCompositionLayer);
    if (mHasCompositionLayer) {
        result.appendFormat("\tfirstIndex: %d, lastIndex: %d, dataSpace: 0x%8x, compressionType: %8x, windowIndex: %d\n",
                            mFirstIndex, mLastIndex, mDataSpace, mCompressionInfo.type, mWindowIndex);
        result.appendFormat("\thandle: %p, acquireFence: %d, releaseFence: %d, skipFlag: %d, SRAM amunt : %d",
                            mTargetBuffer, mAcquireFence, mReleaseFence, mSkipFlag, mHWResourceAmount[TDM_ATTR_SRAM_AMOUNT]);
        if ((mOtfMPP == NULL) && (mM2mMPP == NULL))
            result.appendFormat("\tresource is not assigned\n");
        if (mOtfMPP != NULL)
            result.appendFormat("\tassignedOtfMPP: %s\n", mOtfMPP->mName.c_str());
        if (mM2mMPP != NULL)
            result.appendFormat("\t%s\n", mM2mMPP->mName.c_str());
    }
    if (mTargetBuffer != NULL) {
        uint64_t internal_format = 0;
        mCompressionInfo = getCompressionInfo(mTargetBuffer);
        internal_format = ExynosGraphicBufferMeta::get_internal_format(mTargetBuffer);
        result.appendFormat("\tinternal_format: 0x%" PRIx64 ", compressionType: %8x\n",
                            internal_format, mCompressionInfo.type);
    }
    uint32_t assignedSrcNum = 0;
    if ((mM2mMPP != NULL) &&
        ((assignedSrcNum = mM2mMPP->mAssignedSources.size()) > 0)) {
        result.appendFormat("\tAssigned source num: %d\n", assignedSrcNum);
        result.append("\t");
        for (uint32_t i = 0; i < assignedSrcNum; i++) {
            if (mM2mMPP->mAssignedSources[i]->mSourceType == MPP_SOURCE_LAYER) {
                ExynosLayer *layer = (ExynosLayer *)(mM2mMPP->mAssignedSources[i]);
                result.appendFormat("[%d]layer_%p ", i, layer->mLayerBuffer);
            } else {
                result.appendFormat("[%d]sourceType_%d ", i, mM2mMPP->mAssignedSources[i]->mSourceType);
            }
        }
        result.append("\n");
    }
    result.append("\n");
}

String8 ExynosCompositionInfo::getTypeStr() {
    switch (mType) {
    case COMPOSITION_NONE:
        return String8("COMPOSITION_NONE");
    case COMPOSITION_CLIENT:
        return String8("COMPOSITION_CLIENT");
    case COMPOSITION_EXYNOS:
        return String8("COMPOSITION_EXYNOS");
    default:
        return String8("InvalidType");
    }
}

ExynosDisplay::ExynosDisplay(DisplayIdentifier node)
    : mDisplayId(0),
      mType(HWC_DISPLAY_PRIMARY),
      mIndex(0),
      mDeconNodeName(""),
      mXres(1440),
      mYres(2960),
      mXdpi(25400),
      mYdpi(25400),
      mVsyncPeriod(16666666),
      mVsyncFd(-1),
      mPsrMode(PSR_MAX),
      mDisplayName(""),
      mPlugState(false),
      mHasSingleBuffer(false),
      mClientCompositionInfo(COMPOSITION_CLIENT),
      mExynosCompositionInfo(COMPOSITION_EXYNOS),
      mGeometryChanged(0x0),
      mRenderingState(RENDERING_STATE_NONE),
      mHWCRenderingState(RENDERING_STATE_NONE),
      mLastUpdateTimeStamp(0),
      mDumpCount(0),
      mDefaultDMA(MAX_DECON_DMA_TYPE),
      mLastPresentFence(-1),
      mMaxWindowNum(0),
      mWindowNumUsed(0),
      mBaseWindowIndex(0),
      mNumMaxPriorityAllowed(1),
      mCursorIndex(-1),
      mColorTransformHint(HAL_COLOR_TRANSFORM_IDENTITY),
      mMaxLuminance(0),
      mMaxAverageLuminance(0),
      mMinLuminance(0),
      mHasHdr10PlusLayer(false),
      mColorMode(HAL_COLOR_MODE_NATIVE),
      mNeedSkipPresent(false),
      mNeedSkipValidatePresent(false),
      mIsSkipFrame(false),
      mMaxBrightness(0),
      mVsyncPeriodChangeConstraints{systemTime(SYSTEM_TIME_MONOTONIC), 0},
      mVsyncAppliedTimeLine{false, 0, systemTime(SYSTEM_TIME_MONOTONIC)},
      mConfigRequestState(hwc_request_state_t::SET_CONFIG_STATE_NONE),
      mDesiredConfig(0) {
    mDisplayControl.enableExynosCompositionOptimization = true;
    mDisplayControl.useMaxG2DSrc = false;
    mDisplayControl.earlyStartMPP = true;
    mDisplayControl.adjustDisplayFrame = false;
    mDisplayControl.cursorSupport = false;

    memset(&mHdrTypes, 0, sizeof(mHdrTypes));

    mDisplayConfigs.clear();

    mPowerModeState = HWC2_POWER_MODE_OFF;
    mVsyncState = HWC2_VSYNC_DISABLE;
    mLastModeSwitchTimeStamp = 0;

    mUseDpu = true;

    mType = node.type;
    mIndex = node.index;
    mDisplayId = getDisplayId(mType, mIndex);
    mDisplayName = node.name;
    mDeconNodeName = node.deconNodeName;
    mDisplayInfo.displayIdentifier = node;

    mHpdStatus = false;

    mLayerDumpManager = new LayerDumpManager(this);

    return;
}

ExynosDisplay::~ExynosDisplay() {
#ifdef USE_DQE_INTERFACE
    if (mDquCoefAddr)
        munmap(mDquCoefAddr, sizeof(struct dqeCoef));
    if (mDqeParcelFd >= 0)
        close(mDqeParcelFd);
#endif
    delete mLayerDumpManager;
}

int ExynosDisplay::getId() {
    return mDisplayId;
}

void ExynosDisplay::init(uint32_t maxWindowNum, ExynosMPP *blendingMPP) {
    getDisplayInfo(mDisplayInfo);
    mClientCompositionInfo.init(mDisplayInfo.displayIdentifier, nullptr);
    mExynosCompositionInfo.init(mDisplayInfo.displayIdentifier, blendingMPP);
    initDisplay();

    if (!mUseDpu)
        return;

    /* Initialize information for dpu */
    mDpuData.init(maxWindowNum);
    mLastDpuData.init(maxWindowNum);
    ALOGI("window configs size(%zu)", mDpuData.configs.size());

    if (mUseDynamicRecomp)
        initOneShotTimer();

    if (maxWindowNum != mDisplayInterface->getMaxWindowNum()) {
        DISPLAY_LOGE("%s:: Invalid max window number (maxWindowNum: %d, getMaxWindowNum: %d)",
                     __func__, maxWindowNum, mDisplayInterface->getMaxWindowNum());
        assert(false);
    }

#ifdef USE_DQE_INTERFACE
    if (allocParcelData(&mDqeParcelFd, sizeof(struct dqeCoef)) != NO_ERROR) {
        DISPLAY_LOGE("Fail to allocate DQE Parcel data");
        return;
    }
    mDquCoefAddr = (struct dqeCoef *)mmap(0, sizeof(struct dqeCoef),
                                          PROT_READ | PROT_WRITE, MAP_SHARED, mDqeParcelFd, 0);
    if (mDquCoefAddr == MAP_FAILED) {
        DISPLAY_LOGE("Fail to map DQE Parcel data");
        mDquCoefAddr = NULL;
    }
#endif

    mWorkingVsyncInfo.setVsyncPeriod(static_cast<uint32_t>(mDisplayInterface->getWorkingVsyncPeriod()));

    return;
}

void ExynosDisplay::initDisplay() {
    initCompositionInfo(mClientCompositionInfo);
    initCompositionInfo(mExynosCompositionInfo);

    mGeometryChanged = 0x0;
    mRenderingState = RENDERING_STATE_NONE;
    mCursorIndex = -1;

    clearWinConfigData();
    mLastDpuData.reset();

    if (mDisplayControl.earlyStartMPP == true) {
        for (size_t i = 0; i < mLayers.size(); i++) {
            exynos_image outImage;
            ExynosMPP *m2mMPP = mLayers[i]->mM2mMPP;

            /* Close acquire fence of dst buffer of last frame */
            if ((mLayers[i]->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) &&
                (m2mMPP != NULL) &&
                (m2mMPP->mAssignedDisplayInfo.displayIdentifier.id == mDisplayId) &&
                (m2mMPP->getDstImageInfo(&outImage) == NO_ERROR)) {
                if (m2mMPP->mPhysicalType == MPP_MSC) {
                    mFenceTracer.fence_close(outImage.acquireFenceFd,
                                             mDisplayInfo.displayIdentifier, FENCE_TYPE_DST_ACQUIRE, FENCE_IP_MSC,
                                             "display::initDisplay: MSC outImage.acquireFenceFd");
                } else if (m2mMPP->mPhysicalType == MPP_G2D) {
                    mFenceTracer.fence_close(outImage.acquireFenceFd,
                                             mDisplayInfo.displayIdentifier, FENCE_TYPE_DST_ACQUIRE, FENCE_IP_G2D,
                                             "display::initDisplay: G2D outImage.acquireFenceFd");
                } else {
                    DISPLAY_LOGE("[%zu] layer has invalid mppType(%d), fence fd(%d)",
                                 i, m2mMPP->mPhysicalType, outImage.acquireFenceFd);
                    mFenceTracer.fence_close(outImage.acquireFenceFd,
                                             mDisplayInfo.displayIdentifier, FENCE_TYPE_DST_ACQUIRE, FENCE_IP_ALL);
                }
                m2mMPP->resetDstAcquireFence();
            }
        }
    }
}

void ExynosDisplay::initCompositionInfo(ExynosCompositionInfo &compositionInfo) {
    compositionInfo.initializeInfos();

    if (compositionInfo.mType == COMPOSITION_CLIENT)
        compositionInfo.mEnableSkipStatic = true;
    else
        compositionInfo.mEnableSkipStatic = false;

    compositionInfo.mSkipStaticInitFlag = false;
    compositionInfo.mSkipFlag = false;
    compositionInfo.mSkipSrcInfo.reset();
}

/**
 * @param outLayer
 * @return int32_t
 */
int32_t ExynosDisplay::destroyLayer(hwc2_layer_t outLayer,
                                    uint64_t &geometryFlag) {
    Mutex::Autolock lock(mDRMutex);
    if ((ExynosLayer *)outLayer == NULL)
        return HWC2_ERROR_BAD_LAYER;

    mLayers.remove((ExynosLayer *)outLayer);
    mDisplayInterface->onLayerDestroyed(outLayer);

    delete (ExynosLayer *)outLayer;
    setGeometryChanged(GEOMETRY_DISPLAY_LAYER_REMOVED, geometryFlag);

    if (mPlugState == false) {
        DISPLAY_LOGI("%s : destroyLayer is done. But display is already disconnected",
                     __func__);
        return HWC2_ERROR_BAD_DISPLAY;
    }

    return HWC2_ERROR_NONE;
}

/**
 * @return void
 */
void ExynosDisplay::destroyLayers() {
    while (!mLayers.isEmpty()) {
        ExynosLayer *layer = mLayers[0];
        if (layer != NULL) {
            mLayers.remove(layer);
            delete layer;
        }
    }
}

ExynosLayer *ExynosDisplay::checkLayer(hwc2_layer_t addr, bool printError) {
    ExynosLayer *temp = (ExynosLayer *)addr;
    for (auto layer : mLayers) {
        if (layer == temp)
            return layer;
    }

    if (printError)
        DISPLAY_LOGE("HWC2 : %s wrong layer request, layer num(%zu)!", __func__, mLayers.size());
    return NULL;
}

/**
 * @return void
 */
void ExynosDisplay::doPreProcessing(DeviceValidateInfo &validateInfo,
                                    uint64_t &geometryChanged) {
    /* Low persistence setting */
    int ret = 0;
    uint32_t selfRefresh = 0;
    unsigned int skipProcessing = 1;
    bool hasSingleBuffer = false;
    bool skipStaticLayers = true;

    getDisplayInfo(mDisplayInfo);

    mHasHdr10PlusLayer = false;

    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->updateDisplayInfo(mDisplayInfo);

        buffer_handle_t handle = mLayers[i]->mLayerBuffer;
        ExynosGraphicBufferMeta gmeta(handle);
        /* TODO: This should be checked **/
        if ((handle != NULL) &&
#ifdef GRALLOC_VERSION1
            (gmeta.consumer_usage & ExynosGraphicBufferUsage::DAYDREAM_SINGLE_BUFFER_MODE))
#else
            (gmeta.flags & BufferUsage::DAYDREAM_SINGLE_BUFFER_MODE))
#endif
        {
            hasSingleBuffer = true;
        }
        if (mLayers[i]->mCompositionType == HWC2_COMPOSITION_CLIENT)
            skipStaticLayers = false;

        if (mLayers[i]->doPreProcess(validateInfo, geometryChanged) < 0) {
            DISPLAY_LOGE("%s:: layer.doPreProcess() error, layer %zu", __func__, i);
        }

        if (mLayers[i]->mIsHdr10PlusLayer)
            mHasHdr10PlusLayer = true;

#ifdef NUM_CAMERA_DPP_CHANEL_NUM
        /* camera exception scenario */
        if (i == 0 && handle != NULL && isFormatYUV(gmeta.format) &&
            validateInfo.useCameraException &&
            mLayers[i]->mOverlayPriority != ePriorityMax) {
            mLayers[i]->mOverlayPriority = ePriorityHigh;
            mLayers[i]->setGeometryChanged(GEOMETRY_LAYER_PRIORITY_CHANGED,
                                           mGeometryChanged);
            setGeometryChanged(GEOMETRY_LAYER_PRIORITY_CHANGED, geometryChanged);
        }
#endif

        exynos_image srcImg;
        exynos_image dstImg;
        mLayers[i]->setSrcExynosImage(&srcImg);
        mLayers[i]->setDstExynosImage(&dstImg);
        mLayers[i]->setExynosImage(srcImg, dstImg);
    }

    // Re-align layer priority for max overlay resources
    uint32_t numMaxPriorityLayers = 0;
    for (int i = (mLayers.size() - 1); i >= 0; i--) {
        ExynosLayer *layer = mLayers[i];
        HDEBUGLOGD(eDebugResourceManager, "Priority align: i:%d, layer priority:%d, Max:%d, mNumMaxPriorityAllowed:%d", i,
                   layer->mOverlayPriority, numMaxPriorityLayers, mNumMaxPriorityAllowed);
        if (layer->mOverlayPriority == ePriorityMax) {
            if (numMaxPriorityLayers >= mNumMaxPriorityAllowed) {
                layer->mOverlayPriority = ePriorityHigh;
                layer->setGeometryChanged(GEOMETRY_LAYER_PRIORITY_CHANGED,
                                          mGeometryChanged);
                setGeometryChanged(GEOMETRY_LAYER_PRIORITY_CHANGED,
                                   geometryChanged);
            }
            numMaxPriorityLayers++;
        }
    }

    /*
     * Disable skip static layer feature if there is the layer that's
     * mCompositionType  is HWC2_COMPOSITION_CLIENT
     * HWC should not change compositionType if it is HWC2_COMPOSITION_CLIENT
     */
    if (mType != HWC_DISPLAY_VIRTUAL)
        mClientCompositionInfo.mEnableSkipStatic = skipStaticLayers;

    if (mHasSingleBuffer != hasSingleBuffer) {
        if (hasSingleBuffer) {
            selfRefresh = 1;
            skipProcessing = 0;
        } else {
            selfRefresh = 0;
            skipProcessing = 1;
        }
        if ((ret = mDisplayInterface->disableSelfRefresh(selfRefresh)) < 0)
            DISPLAY_LOGE("ioctl S3CFB_LOW_PERSISTENCE failed: %s ret(%d)", strerror(errno), ret);
        mHasSingleBuffer = hasSingleBuffer;
        mDisplayControl.skipM2mProcessing = skipProcessing;
        mDisplayControl.skipStaticLayers = skipProcessing;
        mDynamicRecompMode = NO_MODE_SWITCH;

        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED, geometryChanged);
        setGeometryChanged(GEOMETRY_DISPLAY_DYNAMIC_RECOMPOSITION,
                           geometryChanged);
        setGeometryChanged(GEOMETRY_DISPLAY_SINGLEBUF_CHANGED, geometryChanged);
    }

    if (mUseDynamicRecomp && mDynamicRecompTimer)
        checkLayersForRevertingDR(geometryChanged);

    /* Display info could be changed */
    getDisplayInfo(mDisplayInfo);
    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->updateDisplayInfo(mDisplayInfo);
    }
    mDisplayInterface->updateDisplayInfo(mDisplayInfo);

    if (mType == HWC_DISPLAY_PRIMARY) {
        mDisplayInterface->canDisableAllPlanes(validateInfo.hasUnstartedDisplay);
    }

    return;
}

void ExynosDisplay::setGeometryChanged(uint64_t changedBit,
                                       uint64_t &outGeometryChanged) {
    mGeometryChanged |= changedBit;
    outGeometryChanged |= changedBit;
}

void ExynosDisplay::clearGeometryChanged() {
    mGeometryChanged = 0;
    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->clearGeometryChanged();
    }
}

int ExynosDisplay::handleStaticLayers(ExynosCompositionInfo &compositionInfo) {
    if (compositionInfo.mType != COMPOSITION_CLIENT)
        return -EINVAL;

    if (mType == HWC_DISPLAY_VIRTUAL)
        return NO_ERROR;

    if (compositionInfo.mHasCompositionLayer == false) {
        DISPLAY_LOGD(eDebugSkipStaicLayer, "there is no client composition");
        return NO_ERROR;
    }
    if ((compositionInfo.mWindowIndex < 0) ||
        (compositionInfo.mWindowIndex >= (int32_t)mDpuData.configs.size())) {
        DISPLAY_LOGE("invalid mWindowIndex(%d)", compositionInfo.mWindowIndex);
        return -EINVAL;
    }

    exynos_win_config_data &config = mDpuData.configs[compositionInfo.mWindowIndex];

    /* Store configuration of client target configuration */
    if (compositionInfo.mSkipFlag == false) {
        compositionInfo.mLastWinConfigData = config;
        DISPLAY_LOGD(eDebugSkipStaicLayer, "config[%d] is stored",
                     compositionInfo.mWindowIndex);
    } else {
        for (size_t i = (size_t)compositionInfo.mFirstIndex; i <= (size_t)compositionInfo.mLastIndex; i++) {
            if ((mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_CLIENT) &&
                (mLayers[i]->mAcquireFence >= 0))
                mFenceTracer.fence_close(mLayers[i]->mAcquireFence,
                                         mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL,
                                         "display::handleStaticLayers: layer acq_fence");
            mLayers[i]->mAcquireFence = -1;
            mLayers[i]->mReleaseFence = -1;
        }

        if (compositionInfo.mTargetBuffer == NULL) {
            mFenceTracer.fence_close(config.acq_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL,
                                     "display::handleStaticLayers: target acq_fence");

            config = compositionInfo.mLastWinConfigData;
            /* Assigned otfMPP for client target can be changed */
            config.assignedMPP = compositionInfo.mOtfMPP;

            if (compositionInfo.mOtfMPP &&
                (setColorConversionInfo(compositionInfo.mOtfMPP) == NO_ERROR))
                config.fd_lut = compositionInfo.mOtfMPP->mLutParcelFd;

            /* acq_fence was closed by DPU driver in the previous frame */
            config.acq_fence = -1;
        } else {
            /* Check target buffer is same with previous frame */
            if (!std::equal(config.fd_idma, config.fd_idma + 3, compositionInfo.mLastWinConfigData.fd_idma)) {
                DISPLAY_LOGE("Current config [%d][%d, %d, %d]",
                             compositionInfo.mWindowIndex,
                             config.fd_idma[0], config.fd_idma[1], config.fd_idma[2]);
                DISPLAY_LOGE("=============================  dump last win configs  ===================================");
                for (size_t i = 0; i < mLastDpuData.configs.size(); i++) {
                    android::String8 result;
                    result.appendFormat("config[%zu]\n", i);
                    dumpConfig(result, mLastDpuData.configs[i]);
                    DISPLAY_LOGE("%s", result.c_str());
                }
                DISPLAY_LOGE("compositionInfo.mLastWinConfigData config [%d, %d, %d]",
                             compositionInfo.mLastWinConfigData.fd_idma[0],
                             compositionInfo.mLastWinConfigData.fd_idma[1],
                             compositionInfo.mLastWinConfigData.fd_idma[2]);
                return -EINVAL;
            }
        }

        DISPLAY_LOGD(eDebugSkipStaicLayer, "skipStaticLayer config[%d]", compositionInfo.mWindowIndex);
        dumpConfig(config);
    }

    return NO_ERROR;
}

bool ExynosDisplay::skipStaticLayerChanged(ExynosCompositionInfo &compositionInfo) {
    if ((int)compositionInfo.mSkipSrcInfo.srcNum !=
        (compositionInfo.mLastIndex - compositionInfo.mFirstIndex + 1)) {
        DISPLAY_LOGD(eDebugSkipStaicLayer, "Client composition number is changed (%d -> %d)",
                     compositionInfo.mSkipSrcInfo.srcNum,
                     compositionInfo.mLastIndex - compositionInfo.mFirstIndex + 1);
        return true;
    }

    bool isChanged = false;
    for (size_t i = (size_t)compositionInfo.mFirstIndex; i <= (size_t)compositionInfo.mLastIndex; i++) {
        ExynosLayer *layer = mLayers[i];
        size_t index = i - compositionInfo.mFirstIndex;
        if ((layer->mLayerBuffer == NULL) ||
            (compositionInfo.mSkipSrcInfo.srcInfo[index].bufferHandle != layer->mLayerBuffer)) {
            isChanged = true;
            DISPLAY_LOGD(eDebugSkipStaicLayer, "layer[%zu] handle is changed"
                                               " handle(%p -> %p), layerFlag(0x%8x)",
                         i, compositionInfo.mSkipSrcInfo.srcInfo[index].bufferHandle,
                         layer->mLayerBuffer, layer->mLayerFlag);
            break;
        } else if ((compositionInfo.mSkipSrcInfo.srcInfo[index].x != layer->mSrcImg.x) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].y != layer->mSrcImg.y) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].w != layer->mSrcImg.w) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].h != layer->mSrcImg.h) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].dataSpace != layer->mSrcImg.dataSpace) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].blending != layer->mSrcImg.blending) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].transform != layer->mSrcImg.transform) ||
                   (compositionInfo.mSkipSrcInfo.srcInfo[index].planeAlpha != layer->mSrcImg.planeAlpha)) {
            isChanged = true;
            DISPLAY_LOGD(eDebugSkipStaicLayer, "layer[%zu] source info is changed, "
                                               "x(%d->%d), y(%d->%d), w(%d->%d), h(%d->%d), dataSpace(%d->%d), "
                                               "blending(%d->%d), transform(%d->%d), planeAlpha(%3.1f->%3.1f)",
                         i,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].x, layer->mSrcImg.x,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].y, layer->mSrcImg.y,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].w, layer->mSrcImg.w,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].h, layer->mSrcImg.h,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].dataSpace, layer->mSrcImg.dataSpace,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].blending, layer->mSrcImg.blending,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].transform, layer->mSrcImg.transform,
                         compositionInfo.mSkipSrcInfo.srcInfo[index].planeAlpha, layer->mSrcImg.planeAlpha);
            break;
        } else if ((compositionInfo.mSkipSrcInfo.dstInfo[index].x != layer->mDstImg.x) ||
                   (compositionInfo.mSkipSrcInfo.dstInfo[index].y != layer->mDstImg.y) ||
                   (compositionInfo.mSkipSrcInfo.dstInfo[index].w != layer->mDstImg.w) ||
                   (compositionInfo.mSkipSrcInfo.dstInfo[index].h != layer->mDstImg.h)) {
            isChanged = true;
            DISPLAY_LOGD(eDebugSkipStaicLayer, "layer[%zu] dst info is changed, "
                                               "x(%d->%d), y(%d->%d), w(%d->%d), h(%d->%d)",
                         i,
                         compositionInfo.mSkipSrcInfo.dstInfo[index].x, layer->mDstImg.x,
                         compositionInfo.mSkipSrcInfo.dstInfo[index].y, layer->mDstImg.y,
                         compositionInfo.mSkipSrcInfo.dstInfo[index].w, layer->mDstImg.w,
                         compositionInfo.mSkipSrcInfo.dstInfo[index].h, layer->mDstImg.h);
            break;
        }
    }
    return isChanged;
}

/**
 * @param compositionType
 * @return int
 */
int ExynosDisplay::skipStaticLayers(ExynosCompositionInfo &compositionInfo) {
    compositionInfo.mSkipFlag = false;

    if (compositionInfo.mType != COMPOSITION_CLIENT)
        return -EINVAL;

    if ((mDisplayControl.skipStaticLayers == 0) ||
        (compositionInfo.mEnableSkipStatic == false)) {
        DISPLAY_LOGD(eDebugSkipStaicLayer, "skipStaticLayers(%d), mEnableSkipStatic(%d)",
                     mDisplayControl.skipStaticLayers, compositionInfo.mEnableSkipStatic);
        compositionInfo.mSkipStaticInitFlag = false;
        return NO_ERROR;
    }

    if ((compositionInfo.mHasCompositionLayer == false) ||
        (compositionInfo.mFirstIndex < 0) ||
        (compositionInfo.mLastIndex < 0) ||
        ((compositionInfo.mLastIndex - compositionInfo.mFirstIndex + 1) > NUM_SKIP_STATIC_LAYER)) {
        DISPLAY_LOGD(eDebugSkipStaicLayer, "mHasCompositionLayer(%d), mFirstIndex(%d), mLastIndex(%d)",
                     compositionInfo.mHasCompositionLayer,
                     compositionInfo.mFirstIndex, compositionInfo.mLastIndex);
        compositionInfo.mSkipStaticInitFlag = false;
        return NO_ERROR;
    }

    if ((mGeometryChanged & GEOMETRY_DISPLAY_COLOR_TRANSFORM_CHANGED) ||
        (mGeometryChanged & GEOMETRY_DISPLAY_LAYER_REMOVED) ||
        (mGeometryChanged & GEOMETRY_DISPLAY_LAYER_ADDED)) {
        DISPLAY_LOGD(eDebugSkipStaicLayer, "geometry is changed 0x%" PRIx64 "",
                     mGeometryChanged);
        compositionInfo.mSkipStaticInitFlag = false;
        return NO_ERROR;
    }

    if (compositionInfo.mSkipStaticInitFlag) {
        bool isChanged = skipStaticLayerChanged(compositionInfo);
        if (isChanged == true) {
            compositionInfo.mSkipStaticInitFlag = false;
            return NO_ERROR;
        }

        for (size_t i = (size_t)compositionInfo.mFirstIndex; i <= (size_t)compositionInfo.mLastIndex; i++) {
            ExynosLayer *layer = mLayers[i];
            if (layer->mValidateCompositionType == COMPOSITION_CLIENT) {
                layer->mOverlayInfo |= eSkipStaticLayer;
            } else {
                compositionInfo.mSkipStaticInitFlag = false;
                if (layer->mOverlayPriority < ePriorityHigh) {
                    DISPLAY_LOGE("[%zu] Invalid layer type: %d",
                                 i, layer->mValidateCompositionType);
                    return -EINVAL;
                } else {
                    return NO_ERROR;
                }
            }
        }

        compositionInfo.mSkipFlag = true;
        DISPLAY_LOGD(eDebugSkipStaicLayer, "SkipStaicLayer is enabled");
        return NO_ERROR;
    }

    compositionInfo.mSkipStaticInitFlag = true;
    compositionInfo.mSkipSrcInfo.reset();

    for (size_t i = (size_t)compositionInfo.mFirstIndex; i <= (size_t)compositionInfo.mLastIndex; i++) {
        ExynosLayer *layer = mLayers[i];
        size_t index = i - compositionInfo.mFirstIndex;
        compositionInfo.mSkipSrcInfo.srcInfo[index] = layer->mSrcImg;
        compositionInfo.mSkipSrcInfo.dstInfo[index] = layer->mDstImg;
        DISPLAY_LOGD(eDebugSkipStaicLayer, "mSkipSrcInfo.srcInfo[%zu] is initialized, %p",
                     index, layer->mSrcImg.bufferHandle);
    }
    compositionInfo.mSkipSrcInfo.srcNum = (compositionInfo.mLastIndex - compositionInfo.mFirstIndex + 1);
    return NO_ERROR;
}

/**
 * @return int
 */
int ExynosDisplay::doPostProcessing() {
    for (size_t i = 0; i < mLayers.size(); i++) {
        /* Layer handle back-up */
        mLayers[i]->mLastLayerBuffer = mLayers[i]->mLayerBuffer;
    }
    clearGeometryChanged();

    return 0;
}

bool ExynosDisplay::validateExynosCompositionLayer() {
    bool isValid = true;
    ExynosMPP *m2mMpp = mExynosCompositionInfo.mM2mMPP;

    int sourceSize = (int)m2mMpp->mAssignedSources.size();
    if ((mExynosCompositionInfo.mFirstIndex >= 0) &&
        (mExynosCompositionInfo.mLastIndex >= 0)) {
        sourceSize = mExynosCompositionInfo.mLastIndex - mExynosCompositionInfo.mFirstIndex + 1;

        if (!mUseDpu && mClientCompositionInfo.mHasCompositionLayer)
            sourceSize++;
    }

    if (m2mMpp->mAssignedSources.size() == 0) {
        DISPLAY_LOGE("No source images");
        isValid = false;
    } else if (mUseDpu && (((mExynosCompositionInfo.mFirstIndex < 0) ||
                            (mExynosCompositionInfo.mLastIndex < 0)) ||
                           (sourceSize != (int)m2mMpp->mAssignedSources.size()))) {
        DISPLAY_LOGE("Invalid index (%d, %d), size(%zu), sourceSize(%d)",
                     mExynosCompositionInfo.mFirstIndex,
                     mExynosCompositionInfo.mLastIndex,
                     m2mMpp->mAssignedSources.size(),
                     sourceSize);
        isValid = false;
    }
    if (isValid == false) {
        for (int32_t i = mExynosCompositionInfo.mFirstIndex; i <= mExynosCompositionInfo.mLastIndex; i++) {
            /* break when only framebuffer target is assigned on ExynosCompositor */
            if (i == -1)
                break;

            if (mLayers[i]->mAcquireFence >= 0)
                mFenceTracer.fence_close(mLayers[i]->mAcquireFence, mDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL,
                                         "display::validateExynosCompositionLayer: layer acq_fence");
            mLayers[i]->mAcquireFence = -1;
        }
        mExynosCompositionInfo.mM2mMPP->requestHWStateChange(MPP_HW_STATE_IDLE);
    }
    return isValid;
}

/**
 * @return int
 */
int ExynosDisplay::doExynosComposition() {
    int ret = NO_ERROR;
    exynos_image src_img;
    exynos_image dst_img;

    if (mExynosCompositionInfo.mHasCompositionLayer) {
        if (mExynosCompositionInfo.mM2mMPP == NULL) {
            DISPLAY_LOGE("mExynosCompositionInfo.mM2mMPP is NULL");
            return -EINVAL;
        }
        mExynosCompositionInfo.mM2mMPP->requestHWStateChange(MPP_HW_STATE_RUNNING);
        /* mAcquireFence is updated, Update image info */
        for (int32_t i = mExynosCompositionInfo.mFirstIndex; i <= mExynosCompositionInfo.mLastIndex; i++) {
            /* break when only framebuffer target is assigned on ExynosCompositor */
            if (i == -1)
                break;

            struct exynos_image srcImg, dstImg;
            mLayers[i]->setSrcExynosImage(&srcImg);
            dumpExynosImage(eDebugMPP, srcImg);
            mLayers[i]->setDstExynosImage(&dstImg);
            mLayers[i]->setExynosImage(srcImg, dstImg);
        }

        /* For debugging */
        if (validateExynosCompositionLayer() == false) {
            DISPLAY_LOGE("mExynosCompositionInfo is not valid");
            return -EINVAL;
        }

        setCompositionTargetExynosImage(COMPOSITION_EXYNOS, &src_img, &dst_img);
        if (mExynosCompositionInfo.mM2mMPP->canUseVotf(src_img)) {
            VotfInfo votfInfo;
            mExynosCompositionInfo.mOtfMPP->enableVotfInfo(votfInfo);
            mExynosCompositionInfo.mM2mMPP->setVotfInfo(votfInfo);
        }

        mExynosCompositionInfo.mM2mMPP->mCurrentTargetCompressionInfoType = mExynosCompositionInfo.mCompressionInfo.type;
        if ((ret = mExynosCompositionInfo.mM2mMPP->doPostProcessing(mExynosCompositionInfo.mSrcImg,
                                                                    mExynosCompositionInfo.mDstImg)) != NO_ERROR) {
            DISPLAY_LOGE("exynosComposition doPostProcessing fail ret(%d)", ret);
            return ret;
        }

        for (int32_t i = mExynosCompositionInfo.mFirstIndex; i <= mExynosCompositionInfo.mLastIndex; i++) {
            /* break when only framebuffer target is assigned on ExynosCompositor */
            if (i == -1)
                break;
            /* This should be closed by resource lib (libmpp or libacryl) */
            mLayers[i]->mAcquireFence = -1;
        }

        exynos_image outImage;
        if ((ret = mExynosCompositionInfo.mM2mMPP->getDstImageInfo(&outImage)) != NO_ERROR) {
            DISPLAY_LOGE("exynosComposition getDstImageInfo fail ret(%d)", ret);
            return ret;
        }

        android_dataspace dataspace = HAL_DATASPACE_UNKNOWN;
        if (mColorMode != HAL_COLOR_MODE_NATIVE)
            dataspace = colorModeToDataspace(mColorMode);
        mExynosCompositionInfo.setTargetBuffer(outImage.bufferHandle,
                                               outImage.acquireFenceFd, dataspace);
        /*
         * buffer handle, dataspace can be changed by setTargetBuffer()
         * ExynosImage should be set again according to changed handle and dataspace
         */
        setCompositionTargetExynosImage(COMPOSITION_EXYNOS, &src_img, &dst_img);
        mExynosCompositionInfo.setExynosImage(src_img, dst_img);

        // Test..
        // setFenceInfo(mExynosCompositionInfo.mAcquireFence, this, "G2D_DST_ACQ", FENCE_FROM);

        if ((ret = mExynosCompositionInfo.mM2mMPP->resetDstAcquireFence()) != NO_ERROR) {
            DISPLAY_LOGE("exynosComposition resetDstAcquireFence fail ret(%d)", ret);
            return ret;
        }
    }

    return ret;
}

bool ExynosDisplay::getHDRException(ExynosLayer *__unused layer,
                                    DevicePresentInfo &__unused deviceInfo) {
    return false;
}

bool ExynosDisplay::is2StepBlendingRequired(exynos_image &src, buffer_handle_t outbuf) {
    return false;
}

int32_t ExynosDisplay::configureHandle(ExynosLayer &layer, int fence_fd,
                                       exynos_win_config_data &cfg, bool hdrException) {
    /* TODO : this is hardcoded */
    int32_t ret = NO_ERROR;
    buffer_handle_t handle = NULL;
    int32_t blending = 0x0100;
    uint32_t x = 0, y = 0;
    uint32_t w = WIDTH(layer.mPreprocessedInfo.displayFrame);
    uint32_t h = HEIGHT(layer.mPreprocessedInfo.displayFrame);
    ExynosMPP *otfMPP = NULL;
    ExynosMPP *m2mMPP = NULL;
    unsigned int luminanceMin = 0;
    unsigned int luminanceMax = 0;

    blending = layer.mBlending;
    otfMPP = layer.mOtfMPP;
    m2mMPP = layer.mM2mMPP;

    cfg.owner = (void *)&layer;
    cfg.compressionInfo = layer.mCompressionInfo;

    if (layer.mCompressionInfo.type != COMP_TYPE_NONE) {
        cfg.comp_src = DPP_COMP_SRC_GPU;
    }
    if (otfMPP == NULL) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: otfMPP is NULL", __func__);
        return -EINVAL;
    }
    if (m2mMPP != NULL)
        handle = m2mMPP->mDstImgs[m2mMPP->mCurrentDstBuf].bufferHandle;
    else
        handle = layer.mLayerBuffer;

    if ((!layer.isDimLayer()) && handle == NULL) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: invalid handle", __func__);
        return -EINVAL;
    }

    if (layer.mPreprocessedInfo.displayFrame.left < 0) {
        unsigned int crop = -layer.mPreprocessedInfo.displayFrame.left;
        DISPLAY_LOGD(eDebugWinConfig, "layer off left side of screen; cropping %u pixels from left edge",
                     crop);
        x = 0;
        w -= crop;
    } else {
        x = layer.mPreprocessedInfo.displayFrame.left;
    }

    if (layer.mPreprocessedInfo.displayFrame.right > (int)mXres) {
        unsigned int crop = layer.mPreprocessedInfo.displayFrame.right - mXres;
        DISPLAY_LOGD(eDebugWinConfig, "layer off right side of screen; cropping %u pixels from right edge",
                     crop);
        w -= crop;
    }

    if (layer.mPreprocessedInfo.displayFrame.top < 0) {
        unsigned int crop = -layer.mPreprocessedInfo.displayFrame.top;
        DISPLAY_LOGD(eDebugWinConfig, "layer off top side of screen; cropping %u pixels from top edge",
                     crop);
        y = 0;
        h -= crop;
    } else {
        y = layer.mPreprocessedInfo.displayFrame.top;
    }

    if (layer.mPreprocessedInfo.displayFrame.bottom > (int)mYres) {
        int crop = layer.mPreprocessedInfo.displayFrame.bottom - mYres;
        DISPLAY_LOGD(eDebugWinConfig, "layer off bottom side of screen; cropping %u pixels from bottom edge",
                     crop);
        h -= crop;
    }

    if ((layer.mExynosCompositionType == HWC2_COMPOSITION_DEVICE) &&
        (layer.mCompositionType == HWC2_COMPOSITION_CURSOR))
        cfg.state = cfg.WIN_STATE_CURSOR;
    else
        cfg.state = cfg.WIN_STATE_BUFFER;
    cfg.dst.x = x;
    cfg.dst.y = y;
    cfg.dst.w = w;
    cfg.dst.h = h;
    cfg.dst.f_w = mXres;
    cfg.dst.f_h = mYres;

    cfg.plane_alpha = layer.mPlaneAlpha;
    cfg.blending = blending;
    cfg.assignedMPP = otfMPP;

    if (layer.isDimLayer()) {
        cfg.state = cfg.WIN_STATE_COLOR;
        hwc_color_t color = layer.mColor;
        cfg.color = (color.a << 24) | (color.r << 16) | (color.g << 8) | color.b;
        DISPLAY_LOGD(eDebugWinConfig, "HWC2: DIM layer is enabled, color: %d, alpha : %f",
                     cfg.color, cfg.plane_alpha);
        return ret;
    }

    ExynosGraphicBufferMeta gmeta(handle);

    if (!layer.mPreprocessedInfo.mUsePrivateFormat)
        cfg.format = layer.mLayerFormat;
    else
        cfg.format = layer.mPreprocessedInfo.mPrivateFormat;

    cfg.fd_idma[0] = gmeta.fd;
    cfg.fd_idma[1] = gmeta.fd1;
    cfg.fd_idma[2] = gmeta.fd2;
    cfg.buffer_id = ExynosGraphicBufferMeta::get_buffer_id(handle);
    cfg.protection = (getDrmMode(gmeta.producer_usage) == SECURE_DRM) ? 1 : 0;

    exynos_image src_img = layer.mSrcImg;

    if (m2mMPP != NULL) {
        DISPLAY_LOGD(eDebugWinConfig, "\tUse m2mMPP, bufIndex: %d", m2mMPP->mCurrentDstBuf);
        dumpExynosImage(eDebugWinConfig, m2mMPP->mAssignedSources[0]->mMidImg);
        exynos_image mpp_dst_img;
        if (m2mMPP->getDstImageInfo(&mpp_dst_img) == NO_ERROR) {
            dumpExynosImage(eDebugWinConfig, mpp_dst_img);
            cfg.src.f_w = mpp_dst_img.fullWidth;
            cfg.src.f_h = mpp_dst_img.fullHeight;
            cfg.src.x = mpp_dst_img.x;
            cfg.src.y = mpp_dst_img.y;
            cfg.src.w = mpp_dst_img.w;
            cfg.src.h = mpp_dst_img.h;
            cfg.format = mpp_dst_img.exynosFormat;
            cfg.acq_fence =
                mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP, mpp_dst_img.acquireFenceFd);

            m2mMPP->resetDstAcquireFence();
        } else {
            HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: Failed to get dst info of m2mMPP", __func__);
        }
        cfg.dataspace = mpp_dst_img.dataSpace;

        cfg.transform = 0;

        if (hasHdrInfo(layer.mMidImg)) {
            uint32_t parcelFdIndex = layer.mMidImg.exynosFormat.bufferNum();
            if (parcelFdIndex > 0 && layer.mMetaParcelFd >= 0) {
                cfg.fd_idma[parcelFdIndex] = layer.mMetaParcelFd;
                cfg.metaParcel = layer.getMetaParcel();
            }

            if (!hdrException)
                cfg.hdr_enable = true;
            else
                cfg.hdr_enable = false;

            /* Min/Max luminance should be set as M2M MPP's HDR operations
             * If HDR is not processed by M2M MPP, M2M's dst image should have source's min/max luminance
             * */
            dstMetaInfo_t metaInfo = m2mMPP->getDstMetaInfo(mpp_dst_img.dataSpace);
            luminanceMin = metaInfo.minLuminance;
            luminanceMax = metaInfo.maxLuminance;
            DISPLAY_LOGD(eDebugMPP, "HWC2: DPP luminance min %d, max %d", luminanceMin, luminanceMax);
        } else {
            cfg.hdr_enable = true;
        }

        src_img = layer.mMidImg;
    } else {
        cfg.src.f_w = src_img.fullWidth;
        cfg.src.f_h = src_img.fullHeight;
        cfg.src.x = layer.mPreprocessedInfo.sourceCrop.left;
        cfg.src.y = layer.mPreprocessedInfo.sourceCrop.top;
        cfg.src.w = WIDTH(layer.mPreprocessedInfo.sourceCrop) - (cfg.src.x - (uint32_t)layer.mPreprocessedInfo.sourceCrop.left);
        cfg.src.h = HEIGHT(layer.mPreprocessedInfo.sourceCrop) - (cfg.src.y - (uint32_t)layer.mPreprocessedInfo.sourceCrop.top);
        cfg.acq_fence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP, fence_fd);

        cfg.dataspace = src_img.dataSpace;
        cfg.transform = src_img.transform;

        if (hasHdrInfo(src_img)) {
            if (!hdrException)
                cfg.hdr_enable = true;
            else
                cfg.hdr_enable = false;

            uint32_t parcelFdIndex = getBufferNumOfFormat(gmeta.format);
            if (parcelFdIndex > 0 && layer.mMetaParcelFd >= 0) {
                cfg.fd_idma[parcelFdIndex] = layer.mMetaParcelFd;
                cfg.metaParcel = layer.getMetaParcel();
            }

            /*
             * Static info uses 0.0001nit unit for luminace
             * Display uses 1nit unit for max luminance
             * and uses 0.0001nit unit for min luminance
             * Conversion is required
             */
            if (src_img.metaParcel != nullptr) {
                luminanceMin = src_img.metaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance;
                luminanceMax = src_img.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance / 10000;
            }
            DISPLAY_LOGD(eDebugMPP, "HWC2: DPP luminance min %d, max %d", luminanceMin, luminanceMax);
        } else {
            cfg.hdr_enable = true;
        }
    }

    cfg.vOtfEnable = otfMPP->mVotfInfo.enable;
    cfg.vOtfBufIndex = otfMPP->mVotfInfo.bufIndex;

    cfg.min_luminance = luminanceMin;
    cfg.max_luminance = luminanceMax;
    cfg.needColorTransform = src_img.needColorTransform;

    /* Adjust configuration */
    uint32_t srcMaxWidth, srcMaxHeight, srcWidthAlign, srcHeightAlign = 0;
    uint32_t srcXAlign, srcYAlign, srcMaxCropWidth, srcMaxCropHeight, srcCropWidthAlign, srcCropHeightAlign = 0;
    srcMaxWidth = otfMPP->getSrcMaxWidth(src_img);
    srcMaxHeight = otfMPP->getSrcMaxHeight(src_img);
    srcWidthAlign = otfMPP->getSrcWidthAlign(src_img);
    srcHeightAlign = otfMPP->getSrcHeightAlign(src_img);
    srcXAlign = otfMPP->getSrcXOffsetAlign(src_img);
    srcYAlign = otfMPP->getSrcYOffsetAlign(src_img);
    srcMaxCropWidth = otfMPP->getSrcMaxCropWidth(src_img);
    srcMaxCropHeight = otfMPP->getSrcMaxCropHeight(src_img);
    srcCropWidthAlign = otfMPP->getSrcCropWidthAlign(src_img);
    srcCropHeightAlign = otfMPP->getSrcCropHeightAlign(src_img);

    if (cfg.src.x < 0)
        cfg.src.x = 0;
    if (cfg.src.y < 0)
        cfg.src.y = 0;

    if (otfMPP != NULL) {
        if (cfg.src.f_w > srcMaxWidth)
            cfg.src.f_w = srcMaxWidth;
        if (cfg.src.f_h > srcMaxHeight)
            cfg.src.f_h = srcMaxHeight;
        cfg.src.f_w = pixel_align_down((unsigned int)cfg.src.f_w, srcWidthAlign);
        cfg.src.f_h = pixel_align_down((unsigned int)cfg.src.f_h, srcHeightAlign);

        cfg.src.x = pixel_align(cfg.src.x, srcXAlign);
        cfg.src.y = pixel_align(cfg.src.y, srcYAlign);
    }

    if (cfg.src.x + cfg.src.w > cfg.src.f_w)
        cfg.src.w = cfg.src.f_w - cfg.src.x;
    if (cfg.src.y + cfg.src.h > cfg.src.f_h)
        cfg.src.h = cfg.src.f_h - cfg.src.y;

    if (otfMPP != NULL) {
        if (cfg.src.w > srcMaxCropWidth)
            cfg.src.w = srcMaxCropWidth;
        if (cfg.src.h > srcMaxCropHeight)
            cfg.src.h = srcMaxCropHeight;
        cfg.src.w = pixel_align_down(cfg.src.w, srcCropWidthAlign);
        cfg.src.h = pixel_align_down(cfg.src.h, srcCropHeightAlign);
    }

    uint64_t bufSize = (uint64_t)gmeta.size * formatToBpp(gmeta.format);
    uint64_t srcSize = (uint64_t)cfg.src.f_w * cfg.src.f_h * cfg.format.bpp();

    if (!isFormatLossy(gmeta.format) && (bufSize < srcSize)) {
        DISPLAY_LOGE("%s:: buffer size is smaller than source size, buf(size: %d, format: %d), src(w: %d, h: %d, format: %s)",
                     __func__, gmeta.size, gmeta.format, cfg.src.f_w, cfg.src.f_h, cfg.format.name().c_str());
        return -EINVAL;
    }

    if (otfMPP && (setColorConversionInfo(otfMPP) == NO_ERROR))
        cfg.fd_lut = otfMPP->mLutParcelFd;

    /* TODO vOTF settings here */

    return ret;
}

int32_t ExynosDisplay::configureOverlay(ExynosLayer *layer,
                                        exynos_win_config_data &cfg, bool hdrException) {
    int32_t ret = NO_ERROR;
    if (layer != NULL) {
        if ((ret = configureHandle(*layer, layer->mAcquireFence, cfg,
                                   hdrException)) != NO_ERROR)
            return ret;

        /* This will be closed by setReleaseFences() using config.acq_fence */
        layer->mAcquireFence = -1;
    }
    return ret;
}
int32_t ExynosDisplay::configureOverlay(ExynosCompositionInfo &compositionInfo) {
    int32_t windowIndex = compositionInfo.mWindowIndex;
    buffer_handle_t handle = compositionInfo.mTargetBuffer;
    ExynosGraphicBufferMeta gmeta(compositionInfo.mTargetBuffer);

    if ((windowIndex < 0) || (windowIndex >= (int32_t)mDpuData.configs.size())) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: ExynosCompositionInfo(%d) has invalid data, windowIndex(%d)",
                 __func__, compositionInfo.mType, windowIndex);
        return -EINVAL;
    }

    exynos_win_config_data &config = mDpuData.configs[windowIndex];
    config.owner = (void *)&compositionInfo;

    if (handle == NULL) {
        /* config will be set by handleStaticLayers */
        if (compositionInfo.mSkipFlag)
            return NO_ERROR;

        if (compositionInfo.mType == COMPOSITION_CLIENT) {
            ALOGW("%s:: ExynosCompositionInfo(%d) has invalid data, handle(%p)",
                  __func__, compositionInfo.mType, handle);
            if (compositionInfo.mAcquireFence >= 0) {
                compositionInfo.mAcquireFence = mFenceTracer.fence_close(compositionInfo.mAcquireFence,
                                                                         mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB,
                                                                         "display::configureOverlay: client comp acq_fence");
            }
            config.state = config.WIN_STATE_DISABLED;
            return NO_ERROR;
        } else {
            HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: ExynosCompositionInfo(%d) has invalid data, handle(%p)",
                     __func__, compositionInfo.mType, handle);
            return -EINVAL;
        }
    }

    config.fd_idma[0] = gmeta.fd;
    config.fd_idma[1] = gmeta.fd1;
    config.fd_idma[2] = gmeta.fd2;
    config.buffer_id = ExynosGraphicBufferMeta::get_buffer_id(compositionInfo.mTargetBuffer);
    config.protection = (getDrmMode(gmeta.producer_usage) == SECURE_DRM) ? 1 : 0;
    config.state = config.WIN_STATE_BUFFER;

    config.assignedMPP = compositionInfo.mOtfMPP;

    config.dst.f_w = mXres;
    config.dst.f_h = mYres;
    if (compositionInfo.mType == COMPOSITION_EXYNOS) {
        config.src.f_w = pixel_align(mXres, GET_M2M_DST_ALIGN(gmeta.format));
        config.src.f_h = pixel_align(mYres, GET_M2M_DST_ALIGN(gmeta.format));
    } else {
        config.src.f_w = gmeta.stride;
        config.src.f_h = gmeta.vstride;
    }
    if (compositionInfo.mCompressionInfo.type ==
        getCompressionInfo(handle).type)
        config.compressionInfo = compositionInfo.mCompressionInfo;
    else
        config.compressionInfo.type = COMP_TYPE_NONE;

    config.format = compositionInfo.mFormat;

    if (compositionInfo.mCompressionInfo.type) {
        if (compositionInfo.mType == COMPOSITION_EXYNOS)
            config.comp_src = DPP_COMP_SRC_G2D;
        else if (compositionInfo.mType == COMPOSITION_CLIENT)
            config.comp_src = DPP_COMP_SRC_GPU;
        else
            HWC_LOGE(mDisplayInfo.displayIdentifier, "unknown composition type: %d", compositionInfo.mType);
    }

    bool useCompositionCrop = true;
    if ((compositionInfo.mHasCompositionLayer) &&
        (compositionInfo.mFirstIndex >= 0) &&
        (compositionInfo.mLastIndex >= 0)) {
        hwc_rect merged_rect, src_rect;
        merged_rect.left = mXres;
        merged_rect.top = mYres;
        merged_rect.right = 0;
        merged_rect.bottom = 0;

        for (int i = compositionInfo.mFirstIndex; i <= compositionInfo.mLastIndex; i++) {
            ExynosLayer *layer = mLayers[i];
            src_rect.left = layer->mDisplayFrame.left;
            src_rect.top = layer->mDisplayFrame.top;
            src_rect.right = layer->mDisplayFrame.right;
            src_rect.bottom = layer->mDisplayFrame.bottom;
            merged_rect = expand(merged_rect, src_rect);
            DISPLAY_LOGD(eDebugWinConfig, "[%d] layer type: [%d, %d] dispFrame [l: %d, t: %d, r: %d, b: %d], mergedRect [l: %d, t: %d, r: %d, b: %d]",
                         i,
                         layer->mCompositionType,
                         layer->mExynosCompositionType,
                         layer->mDisplayFrame.left,
                         layer->mDisplayFrame.top,
                         layer->mDisplayFrame.right,
                         layer->mDisplayFrame.bottom,
                         merged_rect.left,
                         merged_rect.top,
                         merged_rect.right,
                         merged_rect.bottom);
        }

        config.src.x = merged_rect.left;
        config.src.y = merged_rect.top;
        config.src.w = merged_rect.right - merged_rect.left;
        config.src.h = merged_rect.bottom - merged_rect.top;

        ExynosMPP *exynosMPP = config.assignedMPP;
        if (exynosMPP == NULL) {
            DISPLAY_LOGE("%s:: assignedMPP is NULL", __func__);
            useCompositionCrop = false;
        } else {
            /* Check size constraints */
            uint32_t restrictionIdx = getRestrictionIndex(config.format);
            uint32_t srcXAlign = exynosMPP->getSrcXOffsetAlign(restrictionIdx);
            uint32_t srcYAlign = exynosMPP->getSrcYOffsetAlign(restrictionIdx);
            uint32_t srcWidthAlign = exynosMPP->getSrcCropWidthAlign(restrictionIdx);
            uint32_t srcHeightAlign = exynosMPP->getSrcCropHeightAlign(restrictionIdx);
            uint32_t srcMinWidth = exynosMPP->getSrcMinWidth(restrictionIdx);
            uint32_t srcMinHeight = exynosMPP->getSrcMinHeight(restrictionIdx);

            if (config.src.w < srcMinWidth) {
                config.src.x -= (srcMinWidth - config.src.w);
                if (config.src.x < 0)
                    config.src.x = 0;
                config.src.w = srcMinWidth;
            }
            if (config.src.h < srcMinHeight) {
                config.src.y -= (srcMinHeight - config.src.h);
                if (config.src.y < 0)
                    config.src.y = 0;
                config.src.h = srcMinHeight;
            }

            int32_t alignedSrcX = pixel_align_down(config.src.x, srcXAlign);
            int32_t alignedSrcY = pixel_align_down(config.src.y, srcYAlign);
            config.src.w += (config.src.x - alignedSrcX);
            config.src.h += (config.src.y - alignedSrcY);
            config.src.x = alignedSrcX;
            config.src.y = alignedSrcY;
            config.src.w = pixel_align(config.src.w, srcWidthAlign);
            config.src.h = pixel_align(config.src.h, srcHeightAlign);
        }

        config.dst.x = config.src.x;
        config.dst.y = config.src.y;
        config.dst.w = config.src.w;
        config.dst.h = config.src.h;

        if ((config.src.x < 0) ||
            (config.src.y < 0) ||
            ((config.src.x + config.src.w) > mXres) ||
            ((config.src.y + config.src.h) > mYres)) {
            useCompositionCrop = false;
            ALOGW("Invalid composition target crop size: (%d, %d, %d, %d)",
                  config.src.x, config.src.y,
                  config.src.w, config.src.h);
        }

        DISPLAY_LOGD(eDebugWinConfig, "composition(%d) config[%d] x : %d, y : %d, w : %d, h : %d",
                     compositionInfo.mType, windowIndex,
                     config.dst.x, config.dst.y,
                     config.dst.w, config.dst.h);
    } else {
        useCompositionCrop = false;
    }

    if (useCompositionCrop == false) {
        config.src.x = 0;
        config.src.y = 0;
        config.src.w = mXres;
        config.src.h = mYres;
        config.dst.x = 0;
        config.dst.y = 0;
        config.dst.w = mXres;
        config.dst.h = mYres;
    }

    config.blending = HWC2_BLEND_MODE_PREMULTIPLIED;

    config.acq_fence =
        mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP, compositionInfo.mAcquireFence);
    config.plane_alpha = 1;
    config.dataspace = compositionInfo.mSrcImg.dataSpace;
    config.hdr_enable = true;

    /* This will be closed by setReleaseFences() using config.acq_fence */
    compositionInfo.mAcquireFence = -1;
    DISPLAY_LOGD(eDebugSkipStaicLayer, "Configure composition target[%d], config[%d]!!!!",
                 compositionInfo.mType, windowIndex);
    dumpConfig(config);

    uint64_t bufSize = (uint64_t)gmeta.size * formatToBpp(gmeta.format);
    uint64_t srcSize = (uint64_t)config.src.f_w * config.src.f_h * config.format.bpp();
    if (!isFormatLossy(gmeta.format) && (bufSize < srcSize)) {
        DISPLAY_LOGE("%s:: buffer size is smaller than source size, buf(size: %d, format: %d), src(w: %d, h: %d, format: %s)",
                     __func__, gmeta.size, gmeta.format, config.src.f_w, config.src.f_h, config.format.name().c_str());
        return -EINVAL;
    }

    if (compositionInfo.mOtfMPP &&
        (setColorConversionInfo(compositionInfo.mOtfMPP) == NO_ERROR))
        config.fd_lut = compositionInfo.mOtfMPP->mLutParcelFd;

    return NO_ERROR;
}

/**
 * @return int
 */
int ExynosDisplay::setWinConfigData(DevicePresentInfo &deviceInfo) {
    int ret = NO_ERROR;

    if (mClientCompositionInfo.mHasCompositionLayer) {
        if ((ret = configureOverlay(mClientCompositionInfo)) != NO_ERROR) {
            DISPLAY_LOGE("configureOverlay(ClientCompositionInfo) is failed");
            return ret;
        }
    }
    if (mExynosCompositionInfo.mHasCompositionLayer) {
        if ((ret = configureOverlay(mExynosCompositionInfo)) != NO_ERROR) {
            /* TEST */
            //return ret;
            DISPLAY_LOGE("configureOverlay(ExynosCompositionInfo) is failed");
        }
    }

    /* TODO loop for number of layers */
    for (size_t i = 0; i < mLayers.size(); i++) {
        if ((mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_EXYNOS) ||
            (mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_CLIENT))
            continue;
        int32_t windowIndex = mLayers[i]->mWindowIndex;
        if ((windowIndex < 0) || (windowIndex >= (int32_t)mDpuData.configs.size())) {
            DISPLAY_LOGE("%s:: %zu layer has invalid windowIndex(%d)",
                         __func__, i, windowIndex);
            return -EINVAL;
        }
        DISPLAY_LOGD(eDebugWinConfig, "%zu layer, config[%d]", i, windowIndex);
        if ((ret = configureOverlay(mLayers[i], mDpuData.configs[windowIndex],
                                    getHDRException(mLayers[i], deviceInfo))) != NO_ERROR) {
            DISPLAY_LOGE("Fail to configure layer[%zu]", i);
            return ret;
        }
    }

    return 0;
}

void ExynosDisplay::printDebugInfos(String8 &reason) {
    bool writeFile = true;
    FILE *pFile = NULL;
    struct timeval tv;
    struct tm *localTime;
    gettimeofday(&tv, NULL);
    localTime = (struct tm *)localtime((time_t *)&tv.tv_sec);
    reason.appendFormat("errFrameNumber: %" PRId64 " time:%02d-%02d %02d:%02d:%02d.%03lu(%lu)\n",
                        errorFrameCount,
                        localTime->tm_mon + 1, localTime->tm_mday,
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, tv.tv_usec / 1000,
                        ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)));
    ALOGD("%s", reason.c_str());

    if (errorFrameCount >= HWC_PRINT_FRAME_NUM)
        writeFile = false;
    else {
        char filePath[128];
        sprintf(filePath, "%s/%s_hwc_debug%d.dump", ERROR_LOG_PATH0, mDisplayName.c_str(), (int)errorFrameCount);
        pFile = fopen(filePath, "wb");
        if (pFile == NULL) {
            ALOGE("Fail to open file %s, error: %s", filePath, strerror(errno));
            sprintf(filePath, "%s/%s_hwc_debug%d.dump", ERROR_LOG_PATH1, mDisplayName.c_str(), (int)errorFrameCount);
            pFile = fopen(filePath, "wb");
        }
        if (pFile == NULL) {
            ALOGE("Fail to open file %s, error: %s", filePath, strerror(errno));
        } else {
            ALOGI("%s was created", filePath);
            fwrite(reason.c_str(), 1, reason.size(), pFile);
        }
    }
    errorFrameCount++;

    android::String8 result;
    result.appendFormat("%s mGeometryChanged(%" PRIx64 "), mRenderingState(%d)\n",
                        mDisplayName.c_str(), mGeometryChanged, mRenderingState);
    result.appendFormat("=======================  dump composition infos  ================================\n");
    ExynosCompositionInfo clientCompInfo = mClientCompositionInfo;
    ExynosCompositionInfo exynosCompInfo = mExynosCompositionInfo;
    clientCompInfo.dump(result);
    exynosCompInfo.dump(result);
    ALOGD("%s", result.c_str());
    if (pFile != NULL) {
        fwrite(result.c_str(), 1, result.size(), pFile);
    }
    result.clear();

    result.appendFormat("=======================  dump exynos layers (%zu)  ================================\n",
                        mLayers.size());
    ALOGD("%s", result.c_str());
    if (pFile != NULL) {
        fwrite(result.c_str(), 1, result.size(), pFile);
    }
    result.clear();
    for (uint32_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->printLayer();
        if (pFile != NULL) {
            layer->dump(result);
            fwrite(result.c_str(), 1, result.size(), pFile);
            result.clear();
        }
    }

    if (mUseDpu) {
        result.appendFormat("=============================  dump win configs  ===================================\n");
        ALOGD("%s", result.c_str());
        if (pFile != NULL) {
            fwrite(result.c_str(), 1, result.size(), pFile);
        }
        result.clear();
        for (size_t i = 0; i < mDpuData.configs.size(); i++) {
            ALOGD("config[%zu]", i);
            printConfig(mDpuData.configs[i]);
            if (pFile != NULL) {
                result.appendFormat("config[%zu]\n", i);
                dumpConfig(result, mDpuData.configs[i]);
                fwrite(result.c_str(), 1, result.size(), pFile);
                result.clear();
            }
        }
    }

    /* Fence Tracer Information */
    mFenceTracer.printFenceTrace(result, localTime);

    if (pFile != NULL) {
        fclose(pFile);
    }
}

int32_t ExynosDisplay::validateWinConfigData() {
    bool flagValidConfig = true;
    int bufferStateCnt = 0;

    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        exynos_win_config_data &config = mDpuData.configs[i];
        if (config.state == config.WIN_STATE_BUFFER) {
            bool configInvalid = false;
            /* multiple dma mapping */
            for (size_t j = (i + 1); j < mDpuData.configs.size(); j++) {
                exynos_win_config_data &compare_config = mDpuData.configs[j];
                if ((config.state == config.WIN_STATE_BUFFER) &&
                    (compare_config.state == compare_config.WIN_STATE_BUFFER)) {
                    if ((config.assignedMPP != NULL) &&
                        (config.assignedMPP == compare_config.assignedMPP)) {
                        DISPLAY_LOGE("WIN_CONFIG error: duplicated assignedMPP(%s) between win%zu, win%zu",
                                     config.assignedMPP->mName.c_str(), i, j);
                        compare_config.state = compare_config.WIN_STATE_DISABLED;
                        flagValidConfig = false;
                        continue;
                    }
                }
            }
            if ((config.src.x < 0) || (config.src.y < 0) ||
                (config.dst.x < 0) || (config.dst.y < 0) ||
                (config.src.w <= 0) || (config.src.h <= 0) ||
                (config.dst.w <= 0) || (config.dst.h <= 0) ||
                (config.dst.x + config.dst.w > (uint32_t)mXres) ||
                (config.dst.y + config.dst.h > (uint32_t)mYres)) {
                DISPLAY_LOGE("WIN_CONFIG error: invalid pos or size win%zu", i);
                configInvalid = true;
            }

            if ((config.src.w > config.src.f_w) ||
                (config.src.h > config.src.f_h)) {
                DISPLAY_LOGE("WIN_CONFIG error: invalid size %zu, %d, %d, %d, %d", i,
                             config.src.w, config.src.f_w, config.src.h, config.src.f_h);
                configInvalid = true;
            }

            /* Source alignment check */
            ExynosMPP *exynosMPP = config.assignedMPP;
            if (exynosMPP == NULL) {
                DISPLAY_LOGE("WIN_CONFIG error: config %zu assigendMPP is NULL", i);
                configInvalid = true;
            } else {
                uint32_t restrictionIdx = getRestrictionIndex(config.format);
                uint32_t srcXAlign = exynosMPP->getSrcXOffsetAlign(restrictionIdx);
                uint32_t srcYAlign = exynosMPP->getSrcYOffsetAlign(restrictionIdx);
                uint32_t srcWidthAlign = exynosMPP->getSrcCropWidthAlign(restrictionIdx);
                uint32_t srcHeightAlign = exynosMPP->getSrcCropHeightAlign(restrictionIdx);
                if ((config.src.x % srcXAlign != 0) ||
                    (config.src.y % srcYAlign != 0) ||
                    (config.src.w % srcWidthAlign != 0) ||
                    (config.src.h % srcHeightAlign != 0)) {
                    DISPLAY_LOGE("WIN_CONFIG error: invalid src alignment : %zu, "
                                 "assignedMPP: %s, mppType:%d, format(%s), s_x: %d(%d), s_y: %d(%d), s_w : %d(%d), s_h : %d(%d)",
                                 i, config.assignedMPP->mName.c_str(), exynosMPP->mLogicalType, config.format.name().c_str(), config.src.x,
                                 srcXAlign, config.src.y, srcYAlign, config.src.w, srcWidthAlign, config.src.h, srcHeightAlign);
                    configInvalid = true;
                }
            }

            if (configInvalid) {
                config.state = config.WIN_STATE_DISABLED;
                flagValidConfig = false;
            }

            bufferStateCnt++;
        }

        if ((config.state == config.WIN_STATE_COLOR) ||
            (config.state == config.WIN_STATE_CURSOR))
            bufferStateCnt++;
    }

    if (bufferStateCnt == 0) {
        DISPLAY_LOGE("WIN_CONFIG error: has no buffer window");
        flagValidConfig = false;
    }

    if (flagValidConfig)
        return NO_ERROR;
    else
        return -EINVAL;
}

/**
 * @return int
 */
int ExynosDisplay::setDisplayWinConfigData() {
    return 0;
}

bool ExynosDisplay::checkConfigChanged(const exynos_dpu_data &lastConfigsData, const exynos_dpu_data &newConfigsData) {
    for (size_t i = 0; i < lastConfigsData.configs.size(); i++) {
        if ((lastConfigsData.configs[i].state != newConfigsData.configs[i].state) ||
            (lastConfigsData.configs[i].fd_idma[0] != newConfigsData.configs[i].fd_idma[0]) ||
            (lastConfigsData.configs[i].fd_idma[1] != newConfigsData.configs[i].fd_idma[1]) ||
            (lastConfigsData.configs[i].fd_idma[2] != newConfigsData.configs[i].fd_idma[2]) ||
            (lastConfigsData.configs[i].dst.x != newConfigsData.configs[i].dst.x) ||
            (lastConfigsData.configs[i].dst.y != newConfigsData.configs[i].dst.y) ||
            (lastConfigsData.configs[i].dst.w != newConfigsData.configs[i].dst.w) ||
            (lastConfigsData.configs[i].dst.h != newConfigsData.configs[i].dst.h) ||
            (lastConfigsData.configs[i].src.x != newConfigsData.configs[i].src.x) ||
            (lastConfigsData.configs[i].src.y != newConfigsData.configs[i].src.y) ||
            (lastConfigsData.configs[i].src.w != newConfigsData.configs[i].src.w) ||
            (lastConfigsData.configs[i].src.h != newConfigsData.configs[i].src.h) ||
            (lastConfigsData.configs[i].format != newConfigsData.configs[i].format) ||
            (lastConfigsData.configs[i].blending != newConfigsData.configs[i].blending) ||
            (lastConfigsData.configs[i].plane_alpha != newConfigsData.configs[i].plane_alpha))
            return true;
    }

    /* To cover buffer payload changed case */
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mLastLayerBuffer != mLayers[i]->mLayerBuffer)
            return true;
    }

    return false;
}

int ExynosDisplay::canApplyWindowUpdate(const exynos_dpu_data &lastConfigsData, const exynos_dpu_data &newConfigsData, uint32_t index) {
    if ((lastConfigsData.configs[index].state != newConfigsData.configs[index].state) ||
        (lastConfigsData.configs[index].format != newConfigsData.configs[index].format) ||
        (lastConfigsData.configs[index].blending != newConfigsData.configs[index].blending) ||
        (lastConfigsData.configs[index].plane_alpha != newConfigsData.configs[index].plane_alpha)) {
        DISPLAY_LOGD(eDebugWindowUpdate,
                     "damage region is skip, but other configuration except dst was changed");
        DISPLAY_LOGD(eDebugWindowUpdate,
                     "\tstate[%d, %d], format[%s, %s], blending[%d, %d], plane_alpha[%f, %f]",
                     lastConfigsData.configs[index].state, newConfigsData.configs[index].state,
                     lastConfigsData.configs[index].format.name().c_str(),
                     newConfigsData.configs[index].format.name().c_str(),
                     lastConfigsData.configs[index].blending, newConfigsData.configs[index].blending,
                     lastConfigsData.configs[index].plane_alpha, newConfigsData.configs[index].plane_alpha);
        return -1;
    }

    if ((lastConfigsData.configs[index].dst.x != newConfigsData.configs[index].dst.x) ||
        (lastConfigsData.configs[index].dst.y != newConfigsData.configs[index].dst.y) ||
        (lastConfigsData.configs[index].dst.w != newConfigsData.configs[index].dst.w) ||
        (lastConfigsData.configs[index].dst.h != newConfigsData.configs[index].dst.h) ||
        (lastConfigsData.configs[index].src.x != newConfigsData.configs[index].src.x) ||
        (lastConfigsData.configs[index].src.y != newConfigsData.configs[index].src.y) ||
        (lastConfigsData.configs[index].src.w != newConfigsData.configs[index].src.w) ||
        (lastConfigsData.configs[index].src.h != newConfigsData.configs[index].src.h))
        return 1;

    else
        return 0;
}

int ExynosDisplay::setWindowUpdate(const hwc_rect &merge_rect) {
    if (merge_rect.left == 0 && merge_rect.right == (int32_t)mXres &&
        merge_rect.top == 0 && merge_rect.bottom == (int32_t)mYres) {
        DISPLAY_LOGD(eDebugWindowUpdate, "Partial : Full size");
        mDpuData.enable_win_update = true;
        mDpuData.win_update_region.x = 0;
        mDpuData.win_update_region.w = mXres;
        mDpuData.win_update_region.y = 0;
        mDpuData.win_update_region.h = mYres;
        DISPLAY_LOGD(eDebugWindowUpdate, "window update end ------------------");
        return NO_ERROR;
    } else if (merge_rect.left != (int32_t)mXres && merge_rect.right != 0 &&
               merge_rect.top != (int32_t)mYres && merge_rect.bottom != 0) {
        DISPLAY_LOGD(eDebugWindowUpdate, "Partial : %d, %d, %d, %d",
                     merge_rect.left, merge_rect.top, merge_rect.right, merge_rect.bottom);

        mDpuData.enable_win_update = true;
        mDpuData.win_update_region.x = merge_rect.left;
        mDpuData.win_update_region.w = WIDTH(merge_rect);
        mDpuData.win_update_region.y = merge_rect.top;
        mDpuData.win_update_region.h = HEIGHT(merge_rect);
        DISPLAY_LOGD(eDebugWindowUpdate, "window update end ------------------");
        return NO_ERROR;
    } else {
        DISPLAY_LOGD(eDebugWindowUpdate, "Invalid partial rect size");
        return -1;
    }
}

/**
 * @return int
 */
int ExynosDisplay::deliverWinConfigData(DevicePresentInfo &presentInfo) {
    ATRACE_CALL();
    int ret = NO_ERROR;

    ret = validateWinConfigData();
    if (ret != NO_ERROR) {
        DISPLAY_LOGE("%s:: Invalid WIN_CONFIG", __func__);
        return ret;
    }

    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        if (i == DECON_WIN_UPDATE_IDX) {
            DISPLAY_LOGD(eDebugWinConfig | eDebugSkipStaicLayer, "window update config[%zu]", i);
        } else {
            DISPLAY_LOGD(eDebugWinConfig | eDebugSkipStaicLayer, "deliver config[%zu]", i);
        }
        dumpConfig(mDpuData.configs[i]);
    }

    bool canSkipConfig = exynosHWCControl.skipWinConfig;
    if ((presentInfo.vsyncMode != HIGHEST_MODE) ||
        ((presentInfo.nonPrimaryDisplays.size() > 0) &&
         (mType == HWC_DISPLAY_PRIMARY)))
        canSkipConfig = false;

    if (canSkipConfig && (checkConfigChanged(mDpuData, mLastDpuData) == false)) {
        DISPLAY_LOGD(eDebugWinConfig, "Winconfig : same");
#ifndef DISABLE_FENCE
        if (mLastPresentFence > 0) {
            mDpuData.present_fence =
                mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                             mFenceTracer.hwc_dup(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                                  FENCE_TYPE_PRESENT, FENCE_IP_DPP));
        } else
            mDpuData.present_fence = -1;

        for (size_t i = 0; i < mDpuData.configs.size(); i++) {
            mFenceTracer.setFenceInfo(mDpuData.configs[i].acq_fence, mDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP, FENCE_TO);
            if (mDpuData.configs[i].state == mDpuData.configs[i].WIN_STATE_BUFFER)
                mDpuData.configs[i].rel_fence =
                    mFenceTracer.hwc_dup(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP);
        }
#endif
        ret = 0;
    } else {
        bool waitFence = (mDisplayInterface->mType == INTERFACE_TYPE_DRM);
#ifdef WAIT_FENCE
        waitFence = true;
#endif
        if (waitFence) {
            waitPreviousFrameDone(mLastPresentFence);
        } else {
            bool hasExternalDisplay = false;
            for (auto display : presentInfo.nonPrimaryDisplays) {
                if (display.type == HWC_DISPLAY_EXTERNAL) {
                    hasExternalDisplay = true;
                    break;
                }
            }
            if ((presentInfo.vsyncMode == HIGHEST_MODE) ||
                (hasExternalDisplay)) {
                waitPreviousFrameDone(mN2PresentFence);
            }
        }

        for (size_t i = 0; i < mDpuData.configs.size(); i++) {
            mFenceTracer.setFenceInfo(mDpuData.configs[i].acq_fence, mDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP, FENCE_TO);
        }

        if ((ret = mDisplayInterface->deliverWinConfigData(mDpuData)) < 0) {
            DISPLAY_LOGE("%s::interface's deliverWinConfigData() failed: %s, ret(%d)",
                         __func__, strerror(errno), ret);
            return ret;
        } else {
            mLastDpuData = mDpuData;

            /* For inform */
            if ((mHiberState.hiberExitFd != NULL) && (!mHiberState.exitRequested))
                DISPLAY_LOGD(eDebugWinConfig, "Late hiber exit request!");
            initHiberState();
        }

        for (size_t i = 0; i < mDpuData.configs.size(); i++) {
            mFenceTracer.setFenceInfo(mDpuData.configs[i].rel_fence, mDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP, FENCE_FROM);
        }

        if (mDpuData.enable_readback) {
            /*
             * Requtester of readback will get acqFence after presentDisplay
             * so validateFences should not check this fence
             * in presentDisplay so this function sets pendingAllowed parameter.
             */
            mFenceTracer.setFenceInfo(mDpuData.readback_info.acq_fence,
                                      mDisplayInfo.displayIdentifier, FENCE_TYPE_READBACK_ACQUIRE,
                                      FENCE_IP_DPP, FENCE_FROM, true);
        }

        mFenceTracer.setFenceInfo(mDpuData.present_fence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_PRESENT, FENCE_IP_DPP, FENCE_FROM);
    }

    return NO_ERROR;
}

/**
 * @return int
 */
int ExynosDisplay::setReleaseFences() {
    int release_fd = -1;

    /*
     * Close release fence for client target buffer
     * SurfaceFlinger doesn't get release fence for client target buffer
     */
    if ((mClientCompositionInfo.mHasCompositionLayer) &&
        (mClientCompositionInfo.mWindowIndex >= 0) &&
        (mClientCompositionInfo.mWindowIndex < (int32_t)mDpuData.configs.size())) {
        exynos_win_config_data &config = mDpuData.configs[mClientCompositionInfo.mWindowIndex];

        for (int i = mClientCompositionInfo.mFirstIndex; i <= mClientCompositionInfo.mLastIndex; i++) {
            if (mLayers[i]->mExynosCompositionType != HWC2_COMPOSITION_CLIENT) {
                if (mLayers[i]->mOverlayPriority < ePriorityHigh) {
                    HWC_LOGE(mDisplayInfo.displayIdentifier,
                             "%d layer compositionType is not client(%d)\n",
                             i, mLayers[i]->mExynosCompositionType);
                    return -EINVAL;
                } else {
                    continue;
                }
            }
            if (!mUseDpu)
                mLayers[i]->mReleaseFence = -1;
            else
                mLayers[i]->mReleaseFence =
                    mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP,
                                                 mFenceTracer.hwc_dup(config.rel_fence, mDisplayInfo.displayIdentifier,
                                                                      FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP));
        }
        config.rel_fence = mFenceTracer.fence_close(config.rel_fence, mDisplayInfo.displayIdentifier,
                                                    FENCE_TYPE_SRC_RELEASE, FENCE_IP_FB,
                                                    "display::setReleaseFences: config.rel_fence for client comp");
    }

    // DPU doesn't close acq_fence, HWC should close it.
    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        if (mDpuData.configs[i].acq_fence != -1)
            mFenceTracer.fence_close(mDpuData.configs[i].acq_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP,
                                     "display::setReleaseFences: config.acq_fence");
        mDpuData.configs[i].acq_fence = -1;
    }
    // DPU doesn't close rel_fence of readback buffer, HWC should close it
    if (mDpuData.readback_info.rel_fence >= 0) {
        mDpuData.readback_info.rel_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.rel_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                     "display::setReleaseFences: readback rel_fence");
    }
    // DPU doesn't close rel_fence of standalone writeback buffer, HWC should close it
    if (mDpuData.standalone_writeback_info.rel_fence >= 0) {
        mDpuData.standalone_writeback_info.rel_fence =
            mFenceTracer.fence_close(mDpuData.standalone_writeback_info.rel_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_DST_RELEASE, FENCE_IP_OUTBUF,
                                     "display::setReleaseFences: standalone writeback rel_fence");
    }

    for (size_t i = 0; i < mLayers.size(); i++) {
        if ((mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_CLIENT) ||
            (mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_EXYNOS))
            continue;
        if ((mLayers[i]->mWindowIndex < 0) ||
            (mLayers[i]->mWindowIndex >= mDpuData.configs.size())) {
            HWC_LOGE(mDisplayInfo.displayIdentifier,
                     "%s:: layer[%zu] has invalid window index(%d)\n",
                     __func__, i, mLayers[i]->mWindowIndex);
            return -EINVAL;
        }
        exynos_win_config_data &config = mDpuData.configs[mLayers[i]->mWindowIndex];
        if (mLayers[i]->mOtfMPP != NULL) {
            mLayers[i]->mOtfMPP->setHWStateFence(-1);
        }
        if (mLayers[i]->mM2mMPP != NULL) {
            if (mLayers[i]->mM2mMPP->mUseM2MSrcFence)
                mLayers[i]->mReleaseFence = mLayers[i]->mM2mMPP->getSrcReleaseFence(0);
            else {
                mLayers[i]->mReleaseFence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                                                         FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP,
                                                                         mFenceTracer.hwc_dup(config.rel_fence, mDisplayInfo.displayIdentifier,
                                                                                              FENCE_TYPE_SRC_RELEASE, FENCE_IP_LAYER));
            }

            mLayers[i]->mM2mMPP->resetSrcReleaseFence();
#ifdef DISABLE_FENCE
            mLayers[i]->mM2mMPP->setDstReleaseFence(-1);
#else
            if (config.rel_fence >= 0) {
                release_fd = config.rel_fence;
                mFenceTracer.changeFenceInfoState(release_fd, mDisplayInfo.displayIdentifier,
                                                  FENCE_TYPE_DST_RELEASE, FENCE_IP_DPP, FENCE_FROM, true);
                mLayers[i]->mM2mMPP->setDstReleaseFence(release_fd, mDisplayInfo);
            } else {
                DISPLAY_LOGE("config.rel_fence is not valid(%d), layer:%zu, win_index:%d",
                             config.rel_fence, i, mLayers[i]->mWindowIndex);
                mLayers[i]->mM2mMPP->setDstReleaseFence(-1, mDisplayInfo);
            }
#endif
        } else {
#ifdef DISABLE_FENCE
            mLayers[i]->mReleaseFence = -1;
#else
            if (config.rel_fence >= 0) {
                release_fd = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP, config.rel_fence);
                if (release_fd >= 0)
                    mLayers[i]->mReleaseFence = release_fd;
                else {
                    DISPLAY_LOGE("config.rel_fence is not valid(%d), layer:%zu, win_index:%d",
                                 config.rel_fence, i, mLayers[i]->mWindowIndex);
                    mLayers[i]->mReleaseFence = -1;
                }
            } else {
                /* fence for dim layer could be -1 */
                mLayers[i]->mReleaseFence = -1;
            }
#endif
        }
    }

    if (mExynosCompositionInfo.mHasCompositionLayer) {
        if (mExynosCompositionInfo.mM2mMPP == NULL) {
            HWC_LOGE(mDisplayInfo.displayIdentifier,
                     "There is exynos composition, but m2mMPP is NULL\n");
            return -EINVAL;
        }
        if (mUseDpu &&
            ((mExynosCompositionInfo.mWindowIndex < 0) ||
             (mExynosCompositionInfo.mWindowIndex >= (int32_t)mDpuData.configs.size()))) {
            HWC_LOGE(mDisplayInfo.displayIdentifier,
                     "%s:: exynosComposition has invalid window index(%d)\n",
                     __func__, mExynosCompositionInfo.mWindowIndex);
            return -EINVAL;
        }
        exynos_win_config_data &config = mDpuData.configs[mExynosCompositionInfo.mWindowIndex];
        for (int i = mExynosCompositionInfo.mFirstIndex; i <= mExynosCompositionInfo.mLastIndex; i++) {
            /* break when only framebuffer target is assigned on ExynosCompositor */
            if (i == -1)
                break;

            if (mLayers[i]->mExynosCompositionType != HWC2_COMPOSITION_EXYNOS) {
                HWC_LOGE(mDisplayInfo.displayIdentifier,
                         "%d layer compositionType is not exynos(%d)\n",
                         i, mLayers[i]->mExynosCompositionType);
                return -EINVAL;
            }

            if (mExynosCompositionInfo.mM2mMPP->mUseM2MSrcFence)
                mLayers[i]->mReleaseFence =
                    mExynosCompositionInfo.mM2mMPP->getSrcReleaseFence(i - mExynosCompositionInfo.mFirstIndex);
            else {
                mLayers[i]->mReleaseFence =
                    mFenceTracer.hwc_dup(config.rel_fence, mDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_SRC_RELEASE, FENCE_IP_LAYER);
            }
        }
        mExynosCompositionInfo.mM2mMPP->resetSrcReleaseFence();
        if (mUseDpu) {
#ifdef DISABLE_FENCE
            mExynosCompositionInfo.mM2mMPP->setDstReleaseFence(-1);
#else
            if (config.rel_fence >= 0) {
                mFenceTracer.changeFenceInfoState(config.rel_fence,
                                                  mDisplayInfo.displayIdentifier, FENCE_TYPE_DST_RELEASE, FENCE_IP_DPP, FENCE_FROM, true);
                mExynosCompositionInfo.mM2mMPP->setDstReleaseFence(config.rel_fence, mDisplayInfo);
            } else {
                mExynosCompositionInfo.mM2mMPP->setDstReleaseFence(-1, mDisplayInfo);
            }
#endif
        }
    }
    return 0;
}

/**
 * If display uses outbuf and outbuf is invalid, this function return false.
 * Otherwise, this function return true.
 * If outbuf is invalid, display should handle fence of layers.
 */
bool ExynosDisplay::checkFrameValidation() {
    return true;
}

int32_t ExynosDisplay::acceptDisplayChanges() {
    int32_t type = 0;
    if (mRenderingState != RENDERING_STATE_VALIDATED) {
        DISPLAY_LOGE("%s:: display is not validated : %d", __func__, mRenderingState);
        mDisplayInterface->setForcePanic();
        return HWC2_ERROR_NOT_VALIDATED;
    }

    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i] != NULL) {
            HDEBUGLOGD(eDebugDefault, "%s, Layer %zu : %d, %d", __func__, i,
                       mLayers[i]->mExynosCompositionType, mLayers[i]->mValidateCompositionType);
            type = getLayerCompositionTypeForValidationType(i);

            /* update compositionType
             * SF updates their state and doesn't call back into HWC HAL
             */
            mLayers[i]->mCompositionType = type;
            mLayers[i]->mExynosCompositionType = mLayers[i]->mValidateCompositionType;
        } else {
            HWC_LOGE(mDisplayInfo.displayIdentifier, "Layer %zu is NULL", i);
        }
    }
    mRenderingState = RENDERING_STATE_ACCEPTED_CHANGE;
    mHWCRenderingState = RENDERING_STATE_ACCEPTED_CHANGE;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::createLayer(hwc2_layer_t *outLayer,
                                   uint64_t &geometryFlag) {
    Mutex::Autolock lock(mDRMutex);
    if (mPlugState == false) {
        DISPLAY_LOGI("%s : skip createLayer. Display is already disconnected", __func__);
        return HWC2_ERROR_BAD_DISPLAY;
    }

    getDisplayInfo(mDisplayInfo);
    ExynosLayer *layer = new ExynosLayer(mDisplayInfo);
    mLayers.add((ExynosLayer *)layer);
    *outLayer = (hwc2_layer_t)layer;
    setGeometryChanged(GEOMETRY_DISPLAY_LAYER_ADDED, geometryFlag);
    mDisplayInterface->onLayerCreated(*outLayer);

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getActiveConfig(hwc2_config_t *outConfig) {
    Mutex::Autolock lock(mDisplayMutex);
    if (outConfig && mPendConfigInfo.isPending) {
        *outConfig = mPendConfigInfo.config;
        return HWC2_ERROR_NONE;
    }
    return getActiveConfigInternal(outConfig);
}

int32_t ExynosDisplay::getActiveConfigInternal(hwc2_config_t *outConfig) {
    *outConfig = mActiveConfig;
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getLayerCompositionTypeForValidationType(uint32_t layerIndex) {
    int32_t type = -1;

    if (layerIndex >= mLayers.size()) {
        DISPLAY_LOGE("invalid layer index (%d)", layerIndex);
        return type;
    }
    if ((mLayers[layerIndex]->mValidateCompositionType == HWC2_COMPOSITION_CLIENT) &&
        (mClientCompositionInfo.mSkipFlag) &&
        (mClientCompositionInfo.mFirstIndex <= (int32_t)layerIndex) &&
        ((int32_t)layerIndex <= mClientCompositionInfo.mLastIndex)) {
        type = HWC2_COMPOSITION_DEVICE;
    } else if (mLayers[layerIndex]->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS) {
        if (mLayers[layerIndex]->mCompositionType == HWC2_COMPOSITION_SOLID_COLOR)
            type = HWC2_COMPOSITION_SOLID_COLOR;
        else
            type = HWC2_COMPOSITION_DEVICE;
    } else if ((mLayers[layerIndex]->mCompositionType == HWC2_COMPOSITION_CURSOR) &&
               (mLayers[layerIndex]->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)) {
        if (mDisplayControl.cursorSupport == true)
            type = HWC2_COMPOSITION_CURSOR;
        else
            type = HWC2_COMPOSITION_DEVICE;
    } else if ((mLayers[layerIndex]->mCompositionType == HWC2_COMPOSITION_SOLID_COLOR) &&
               (mLayers[layerIndex]->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)) {
        type = HWC2_COMPOSITION_SOLID_COLOR;
    } else {
        type = mLayers[layerIndex]->mValidateCompositionType;
    }

    return type;
}

int32_t ExynosDisplay::getChangedCompositionTypes(
    uint32_t *outNumElements, hwc2_layer_t *outLayers,
    int32_t * /*hwc2_composition_t*/ outTypes) {
    if (mNeedSkipValidatePresent) {
        *outNumElements = 0;
        return HWC2_ERROR_NONE;
    }

    uint32_t count = 0;
    int32_t type = 0;

    for (size_t i = 0; i < mLayers.size(); i++) {
        DISPLAY_LOGD(eDebugHWC, "[%zu] layer: mCompositionType(%d), mValidateCompositionType(%d), mExynosCompositionType(%d), skipFlag(%d)",
                     i, mLayers[i]->mCompositionType, mLayers[i]->mValidateCompositionType,
                     mLayers[i]->mExynosCompositionType, mClientCompositionInfo.mSkipFlag);

        type = getLayerCompositionTypeForValidationType(i);
        if (type != mLayers[i]->mSfCompositionType) {
            if (outLayers == NULL || outTypes == NULL) {
                count++;
            } else {
                if (count < *outNumElements) {
                    outLayers[count] = (hwc2_layer_t)mLayers[i];
                    outTypes[count] = type;
                    count++;
                } else {
                    DISPLAY_LOGE("array size is not valid (%d, %d)", count, *outNumElements);
                    String8 errString;
                    errString.appendFormat("array size is not valid (%d, %d)", count, *outNumElements);
                    printDebugInfos(errString);
                    return HWC2_ERROR_BAD_PARAMETER;
                }
            }
        }
    }

    if ((outLayers == NULL) || (outTypes == NULL))
        *outNumElements = count;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getClientTargetSupport(
    uint32_t width, uint32_t height,
    int32_t /*android_pixel_format_t*/ format,
    int32_t /*android_dataspace_t*/ dataspace) {
    if (width != mXres)
        return HWC2_ERROR_UNSUPPORTED;
    if (height != mYres)
        return HWC2_ERROR_UNSUPPORTED;
    if (format != HAL_PIXEL_FORMAT_RGBA_8888)
        return HWC2_ERROR_UNSUPPORTED;
    if (dataspace != HAL_DATASPACE_UNKNOWN)
        return HWC2_ERROR_UNSUPPORTED;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getColorModes(
    uint32_t *outNumModes, int32_t * /*android_color_mode_t*/ outModes,
    bool canProcessWCG) {
#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDisplayColorInterface) {
        if (outModes == nullptr) {
            mDisplayColorModes.clear();
            std::vector<DisplayColorMode> list = mDisplayColorInterface->getColorModes();
            for (auto mode : list) {
                if ((canProcessWCG == true) || (mode.gamutId == HAL_COLOR_MODE_NATIVE))
                    mDisplayColorModes[mode.modeId] = mode;
            }
            if (mDisplayColorModes.size() == 0) {
                DisplayColorMode native = {HAL_COLOR_MODE_NATIVE, HAL_COLOR_MODE_NATIVE, "NATIVE"};
                mDisplayColorModes.insert(std::make_pair(HAL_COLOR_MODE_NATIVE, native));
            }
            *outNumModes = mDisplayColorModes.size();
        } else {
            uint32_t index = 0;
            for (auto mode : mDisplayColorModes) {
                outModes[index++] = mode.second.modeId;
                ALOGI("[%s] %s:: Supported color modes [%u, %u] (%s)", mDisplayName.c_str(), __func__,
                      mode.second.modeId, mode.second.gamutId, mode.second.modeName.c_str());
            }
        }
        return HWC2_ERROR_NONE;
    }
#endif
    if (canProcessWCG == false) {
        if (outModes == NULL)
            *outNumModes = 1;
        else
            outModes[0] = HAL_COLOR_MODE_NATIVE;
        return HWC2_ERROR_NONE;
    }
    return mDisplayInterface->getColorModes(outNumModes, outModes);
}

int32_t ExynosDisplay::getDisplayAttribute(
    hwc2_config_t config,
    int32_t /*hwc2_attribute_t*/ attribute, int32_t *outValue) {
    auto it = mDisplayConfigs.find(config);
    if (it == mDisplayConfigs.end()) {
        ALOGE("%s:: Can't find config(%d)", __func__, config);
        return HWC2_ERROR_BAD_CONFIG;
    }

    switch (attribute) {
    case HWC2_ATTRIBUTE_VSYNC_PERIOD:
        *outValue = it->second.vsyncPeriod;
        break;

    case HWC2_ATTRIBUTE_WIDTH:
        *outValue = it->second.width;
        break;

    case HWC2_ATTRIBUTE_HEIGHT:
        *outValue = it->second.height;
        break;

    case HWC2_ATTRIBUTE_DPI_X:
        *outValue = it->second.Xdpi;
        break;

    case HWC2_ATTRIBUTE_DPI_Y:
        *outValue = it->second.Ydpi;
        break;

    case HWC2_ATTRIBUTE_CONFIG_GROUP:
        *outValue = it->second.groupId;
        break;

    default:
        DISPLAY_LOGE("unknown display attribute %u", attribute);
        return HWC2_ERROR_BAD_CONFIG;
    }

    if ((attribute != HWC2_ATTRIBUTE_CONFIG_GROUP) && (*outValue <= 0)) {
        ALOGE("%s:: outValue is 0, attribute(%d), config(%d)",
              __func__, attribute, config);
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getDisplayConfigs(
    uint32_t *outNumConfigs,
    hwc2_config_t *outConfigs) {
    return mDisplayInterface->getDisplayConfigs(outNumConfigs, outConfigs, mDisplayConfigs);
}

int32_t ExynosDisplay::getDisplayName(uint32_t *outSize, char *outName) {
    uint32_t strSize = static_cast<uint32_t>(mDisplayName.size());

    if (outName == NULL) {
        *outSize = strSize;
        return HWC2_ERROR_NONE;
    }

    if (*outSize < strSize) {
        DISPLAY_LOGE("Invalide outSize(%d), mDisplayName.size(%d)", *outSize, strSize);
        strSize = *outSize;
    }
    std::strncpy(outName, mDisplayName, strSize);
    *outSize = strSize;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getDisplayRequests(
    int32_t * /*hwc2_display_request_t*/ outDisplayRequests,
    uint32_t *outNumElements, hwc2_layer_t *outLayers,
    int32_t * /*hwc2_layer_request_t*/ outLayerRequests) {
    /*
     * This function doesn't check mRenderingState
     * This can be called in the below rendering state
     * RENDERING_STATE_PRESENTED: when it is called by canSkipValidate()
     * RENDERING_STATE_ACCEPTED_CHANGE: when it is called by SF
     * RENDERING_STATE_VALIDATED:  when it is called by validateDisplay()
     */

    String8 errString;
    if (outDisplayRequests != nullptr)
        *outDisplayRequests = 0;

    /*
     * There was the case that this function was called after
     * HWC called hotplug callback function with disconnect parameter,
     * and all of layers are destroyed.
     * This function checks the number of layers to check this case and
     * just returns in this case.
     */
    if ((mNeedSkipValidatePresent) || (mLayers.size() == 0)) {
        *outNumElements = 0;
        return HWC2_ERROR_NONE;
    }

    auto handle_err = [=, this, &errString]() -> int32_t {
        printDebugInfos(errString);
        mDisplayInterface->setForcePanic();
        return HWC_HAL_ERROR_INVAL;
    };

    uint32_t requestNum = 0;
    if (mClientCompositionInfo.mHasCompositionLayer == true) {
        if ((mClientCompositionInfo.mFirstIndex < 0) ||
            (mClientCompositionInfo.mFirstIndex >= (int)mLayers.size()) ||
            (mClientCompositionInfo.mLastIndex < 0) ||
            (mClientCompositionInfo.mLastIndex >= (int)mLayers.size())) {
            errString.appendFormat("%s:: mClientCompositionInfo.mHasCompositionLayer is true "
                                   "but index is not valid (firstIndex: %d, lastIndex: %d)\n",
                                   __func__, mClientCompositionInfo.mFirstIndex,
                                   mClientCompositionInfo.mLastIndex);
            *outNumElements = 0;
            return handle_err();
        }

        for (int32_t i = mClientCompositionInfo.mFirstIndex; i < mClientCompositionInfo.mLastIndex; i++) {
            ExynosLayer *layer = mLayers[i];
            if ((layer->mPlaneAlpha == 1.0f) && (layer->mOverlayPriority >= ePriorityHigh)) {
                if ((outLayers != NULL) && (outLayerRequests != NULL)) {
                    if (requestNum >= *outNumElements) {
                        errString.appendFormat("%s:: requestNum(%d), *outNumElements(%d)",
                                               __func__, requestNum, *outNumElements);
                        handle_err();
                        return HWC2_ERROR_BAD_PARAMETER;
                    }
                    outLayers[requestNum] = (hwc2_layer_t)layer;
                    outLayerRequests[requestNum] = HWC2_LAYER_REQUEST_CLEAR_CLIENT_TARGET;
                }
                requestNum++;
            }
        }
    }
    if ((outLayers == NULL) || (outLayerRequests == NULL))
        *outNumElements = requestNum;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getDisplayType(
    int32_t * /*hwc2_display_type_t*/ outType) {
    switch (mType) {
    case HWC_DISPLAY_PRIMARY:
    case HWC_DISPLAY_EXTERNAL:
        *outType = HWC2_DISPLAY_TYPE_PHYSICAL;
        return HWC2_ERROR_NONE;
    case HWC_DISPLAY_VIRTUAL:
        *outType = HWC2_DISPLAY_TYPE_VIRTUAL;
        return HWC2_ERROR_NONE;
    default:
        DISPLAY_LOGE("Invalid display type(%d)", mType);
        *outType = HWC2_DISPLAY_TYPE_INVALID;
        return HWC2_ERROR_NONE;
    }
}

int32_t ExynosDisplay::getDozeSupport(
    int32_t *outSupport) {
    if (mDisplayInterface->isDozeModeAvailable()) {
        *outSupport = 1;
    } else {
        *outSupport = 0;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getReleaseFences(
        uint32_t* outNumElements,
        hwc2_layer_t* outLayers, int32_t* outFences) {

    if (outNumElements == NULL) {
        return HWC2_ERROR_BAD_PARAMETER;
    }

    Mutex::Autolock lock(mDisplayMutex);
    uint32_t deviceLayerNum = 0;
    if (outLayers != NULL && outFences != NULL) {
        // second pass call
        for (size_t i = 0; i < mLayers.size(); i++) {
            if (mLayers[i]->mReleaseFence >= 0) {
                if (deviceLayerNum < *outNumElements) {
                    // transfer fence ownership to the caller
                    outLayers[deviceLayerNum] = (hwc2_layer_t)mLayers[i];
                    outFences[deviceLayerNum] = mLayers[i]->mReleaseFence;
                    mLayers[i]->mReleaseFence = -1;

                    DISPLAY_LOGD(eDebugHWC, "[%zu] layer deviceLayerNum(%d), release fence: %d", i,
                                 deviceLayerNum, outFences[deviceLayerNum]);
                } else {
                    // *outNumElements is not from the first pass call.
                    DISPLAY_LOGE("%s: outNumElements %d too small", __func__, *outNumElements);
                    return HWC2_ERROR_BAD_PARAMETER;
                }
                deviceLayerNum++;
            }
        }
    } else {
        // first pass call
        for (size_t i = 0; i < mLayers.size(); i++) {
            if (mLayers[i]->mReleaseFence >= 0) {
                deviceLayerNum++;
            }
        }
    }
    *outNumElements = deviceLayerNum;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::canSkipValidate() {
    if (exynosHWCControl.skipResourceAssign == 0)
        return SKIP_ERR_CONFIG_DISABLED;

    /* This is first frame. validateDisplay can't be skipped */
    if (mRenderingState == RENDERING_STATE_NONE)
        return SKIP_ERR_FIRST_FRAME;

    if (mClientCompositionInfo.mHasCompositionLayer) {
        /*
         * SurfaceFlinger doesn't skip validateDisplay when there is
         * client composition layer.
         */
        return SKIP_ERR_HAS_CLIENT_COMP;
    } else {
        for (uint32_t i = 0; i < mLayers.size(); i++) {
            if (getLayerCompositionTypeForValidationType(i) ==
                HWC2_COMPOSITION_CLIENT) {
                return SKIP_ERR_HAS_CLIENT_COMP;
            }
        }

        if ((mClientCompositionInfo.mSkipStaticInitFlag == true) &&
            (mClientCompositionInfo.mSkipFlag == true)) {
            if (skipStaticLayerChanged(mClientCompositionInfo) == true)
                return SKIP_ERR_SKIP_STATIC_CHANGED;
        }

        /*
         * If there is hwc2_layer_request_t
         * validateDisplay() can't be skipped
         */
        uint32_t outNumRequests = 0;
        if ((getDisplayRequests(nullptr, &outNumRequests, NULL, NULL) != NO_ERROR) ||
            (outNumRequests != 0))
            return SKIP_ERR_HAS_REQUEST;
    }
    return NO_ERROR;
}

int32_t ExynosDisplay::handleSkipPresent(int32_t *outPresentFence) {
    int ret = HWC2_ERROR_NONE;
    if ((mNeedSkipPresent == false) && (mNeedSkipValidatePresent == false))
        return -1;

    ALOGI("[%d] presentDisplay is skipped by mNeedSkipPresent(%d), mNeedSkipValidatePresent(%d)",
          mDisplayId, mNeedSkipPresent, mNeedSkipValidatePresent);
    closeFencesForSkipFrame(RENDERING_STATE_PRESENTED);
    *outPresentFence = -1;
    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->mReleaseFence = -1;
    }
    if (mNeedSkipValidatePresent) {
        ALOGD("\t%s: This display might have been turned on after first validate time",
              mDisplayName.c_str());
        ret = HWC2_ERROR_NONE;
    } else if (mRenderingState == RENDERING_STATE_NONE) {
        ALOGD("\t%s: present has been called without validate after power on - 1",
              mDisplayName.c_str());
        ret = HWC2_ERROR_NONE;
    } else {
        /* mRenderingState == RENDERING_STATE_PRESENTED */
        ALOGD("\t%s: present has been called without validate after power on - 2",
              mDisplayName.c_str());
        ret = HWC2_ERROR_NOT_VALIDATED;
    }

    return ret;
}

int32_t ExynosDisplay::forceSkipPresentDisplay(int32_t *outPresentFence) {
    ATRACE_CALL();
    gettimeofday(&updateTimeInfo.lastPresentTime, NULL);

    int ret = 0;
    HDEBUGLOGD(eDebugResourceManager,
               "%s present is forced to be skipped",
               mDisplayName.c_str());

    closeFencesForSkipFrame(RENDERING_STATE_PRESENTED);
    *outPresentFence = -1;

    for (size_t i = 0; i < mLayers.size(); i++) {
        mLayers[i]->mReleaseFence = -1;
    }
    mRenderingState = RENDERING_STATE_PRESENTED;

    /* Clear presentFlag */
    mRenderingStateFlags.presentFlag = false;

    return ret;
}

int32_t ExynosDisplay::handlePresentError(String8 &errString,
                                          int32_t *outPresentFence) {
    printDebugInfos(errString);
    closeFences();
    clearDisplay();
    *outPresentFence = -1;
    mLastPresentFence = -1;

    mLastDpuData.reset();

    mClientCompositionInfo.mSkipStaticInitFlag = false;
    mExynosCompositionInfo.mSkipStaticInitFlag = false;

    clearWinConfigData();

    mDisplayInterface->setForcePanic();
    return HWC_HAL_ERROR_INVAL;
}

int32_t ExynosDisplay::presentDisplay(DevicePresentInfo &presentInfo,
                                      int32_t *outPresentFence) {
    ATRACE_CALL();
    gettimeofday(&updateTimeInfo.lastPresentTime, NULL);

    int32_t ret = HWC2_ERROR_NONE;
    String8 errString;
    thread_local bool setTaskProfileDone = false;

    if (setTaskProfileDone == false) {
        if (!SetTaskProfiles(gettid(), {"SFMainPolicy"})) {
            ALOGW("Failed to add `%d` into SFMainPolicy", gettid());
        }
        setTaskProfileDone = true;
    }

    Mutex::Autolock lock(mDisplayMutex);

    /*
     * buffer handle, dataspace were set by setClientTarget() after validateDisplay
     * ExynosImage should be set again according to changed handle and dataspace
     */
    exynos_image src_img;
    exynos_image dst_img;
    setCompositionTargetExynosImage(COMPOSITION_CLIENT, &src_img, &dst_img);
    mClientCompositionInfo.setExynosImage(src_img, dst_img);
    mClientCompositionInfo.setExynosMidImage(dst_img);

    /* Validate was skipped */
    if (mRenderingState != RENDERING_STATE_ACCEPTED_CHANGE) {
        if ((ret = updateColorConversionInfo()) != NO_ERROR) {
            DISPLAY_LOGE("%s:: updateColorConversionInfo() fail, ret(%d)",
                         __func__, ret);
            return ret;
        }
    }

    bool needReadWorkingPeriod = false;
    if (mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_PENDING) {
        needReadWorkingPeriod = (mWorkingVsyncInfo.vsyncPeriod) ? false : true;
        if ((ret = doDisplayConfigPostProcess()) != NO_ERROR) {
            DISPLAY_LOGE("doDisplayConfigPostProcess error (%d)", ret);
        }
        /* Working fps could be changed after this frame request */
        if (mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_REQUESTED)
            needReadWorkingPeriod = true;
    }

    if (mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_REQUESTED) {
        /* Do not update mVsyncPeriod */
        updateInternalDisplayConfigVariables(mDesiredConfig, false);
    }

    if ((mLayers.size() == 0) &&
        (mType != HWC_DISPLAY_VIRTUAL)) {
        if (presentInfo.isBootFinished)
            clearDisplay();
        *outPresentFence = -1;
        mLastPresentFence = mFenceTracer.fence_close(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                                     "display::presentDisplay: mLastPresentFence(layer.size==0)");
        return HWC2_ERROR_NONE;
    }

    if (!checkFrameValidation()) {
        clearDisplay();
        *outPresentFence = -1;
        mLastPresentFence = mFenceTracer.fence_close(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                                     "display::presentDisplay: mLastPresentFence in error case");
        return HWC2_ERROR_NONE;
    }

    if ((mDisplayControl.earlyStartMPP == false) &&
        ((ret = startPostProcessing()) != NO_ERROR)) {
        DISPLAY_LOGE("startPostProcessing fail (%d)\n",
                     ret);
        return ret;
    }

    if (mLayerDumpManager->isRunning()) {
        int32_t dumpFrameCurIndex = mLayerDumpManager->getDumpFrameIndex();
        int32_t dumpFrameMaxIndex = mLayerDumpManager->getDumpMaxIndex();
        if (dumpFrameCurIndex == dumpFrameMaxIndex) {
            dumpLayers();
        } else {
            mLayerDumpManager->triggerDumpFrame();
        }
    }

    if (mUseDynamicRecomp && mDynamicRecompTimer &&
        (mDynamicRecompMode != DEVICE_TO_CLIENT))
        mDynamicRecompTimer->reset();

    // loop for all layer
    for (size_t i = 0; i < mLayers.size(); i++) {
        /* mAcquireFence is updated, Update image info */
        struct exynos_image srcImg, dstImg;
        mLayers[i]->setSrcExynosImage(&srcImg);
        mLayers[i]->setDstExynosImage(&dstImg);
        mLayers[i]->setExynosImage(srcImg, dstImg);

        if (mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_CLIENT) {
            mLayers[i]->mReleaseFence = -1;
            mLayers[i]->mAcquireFence =
                mFenceTracer.fence_close(mLayers[i]->mAcquireFence, mDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER,
                                         "display::presentDisplay: client comp layer acq_fence");
        } else if (mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_EXYNOS) {
            continue;
        } else {
            if (mLayers[i]->mOtfMPP != NULL) {
                mLayers[i]->mOtfMPP->requestHWStateChange(MPP_HW_STATE_RUNNING);
            }
        }
    }

    if ((ret = setWinConfigData(presentInfo)) != NO_ERROR) {
        DISPLAY_LOGE("setWinConfigData fail (%d)\n", ret);
        return ret;
    }

    if ((ret = handleStaticLayers(mClientCompositionInfo)) != NO_ERROR) {
        mClientCompositionInfo.mSkipStaticInitFlag = false;
        DISPLAY_LOGE("handleStaticLayers error\n");
        return ret;
    }

    handleWindowUpdate();

    setDisplayWinConfigData();

    if ((ret = deliverWinConfigData(presentInfo)) != NO_ERROR) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: fail to deliver win_config (%d)", __func__, ret);
        if (mDpuData.present_fence > 0)
            mFenceTracer.fence_close(mDpuData.present_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                     "display::presentDisplay: mDpuData.present_fence in error case");
        mDpuData.present_fence = -1;
        return ret;
    }

    if (needReadWorkingPeriod)
        mWorkingVsyncInfo.setVsyncPeriod(static_cast<uint32_t>(mDisplayInterface->getWorkingVsyncPeriod()));

    if ((ret = setReleaseFences()) != NO_ERROR) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: setReleaseFences error",
                 __func__);
        return ret;
    }

    if (mDpuData.present_fence != -1) {
#ifdef DISABLE_FENCE
        if (mDpuData.present_fence >= 0)
            mFenceTracer.fence_close(mDpuData.present_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_PRESENT, FENCE_IP_DPP);
        *outPresentFence = -1;
#else
        *outPresentFence =
            mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_PRESENT, FENCE_IP_DPP, mDpuData.present_fence);
#endif
        mFenceTracer.setFenceInfo(mDpuData.present_fence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_PRESENT, FENCE_IP_LAYER, FENCE_TO);
    } else
        *outPresentFence = -1;

    /* Update last present fence */
    mN2PresentFence = mFenceTracer.fence_close(mN2PresentFence, mDisplayInfo.displayIdentifier,
                                               FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                               "display::presentDisplay: mN2PresentFence wait done");
    mN2PresentFence = mFenceTracer.hwc_dup(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                           FENCE_TYPE_PRESENT, FENCE_IP_DPP);
    mFenceTracer.changeFenceInfoState(mN2PresentFence, mDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_PRESENT, FENCE_IP_DPP, FENCE_DUP, true);

    mLastPresentFence = mFenceTracer.fence_close(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                                 "display::presentDisplay: mLastPresentFence for update");
    mLastPresentFence = mFenceTracer.hwc_dup((*outPresentFence), mDisplayInfo.displayIdentifier,
                                             FENCE_TYPE_PRESENT, FENCE_IP_DPP);
    mFenceTracer.changeFenceInfoState(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_PRESENT, FENCE_IP_DPP, FENCE_DUP, true);

    increaseMPPDstBufIndex();

    /* Check all of acquireFence are closed */
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mAcquireFence != -1) {
            DISPLAY_LOGE("layer[%zu] fence(%d) type(%d, %d, %d) is not closed",
                         i, mLayers[i]->mAcquireFence,
                         mLayers[i]->mCompositionType,
                         mLayers[i]->mExynosCompositionType,
                         mLayers[i]->mValidateCompositionType);
            if (mLayers[i]->mM2mMPP != NULL)
                DISPLAY_LOGE("\t%s is assigned", mLayers[i]->mM2mMPP->mName.c_str());
            /* Fences would be closed by handlePresentError */
            return -EINVAL;
        }
    }
    if (mExynosCompositionInfo.mAcquireFence >= 0) {
        DISPLAY_LOGE("mExynosCompositionInfo mAcquireFence(%d) is not initialized", mExynosCompositionInfo.mAcquireFence);
        mFenceTracer.fence_close(mExynosCompositionInfo.mAcquireFence, mDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_G2D,
                                 "display::presentDisplay: exynos comp mAcquireFence in error case");
        mExynosCompositionInfo.mAcquireFence = -1;
    }
    if (mClientCompositionInfo.mAcquireFence >= 0) {
        DISPLAY_LOGE("mClientCompositionInfo mAcquireFence(%d) is not initialized", mClientCompositionInfo.mAcquireFence);
        mFenceTracer.fence_close(mClientCompositionInfo.mAcquireFence, mDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB,
                                 "display::presentDisplay: client comp mAcquireFence in error case");
        mClientCompositionInfo.mAcquireFence = -1;
    }

    /* All of release fences are tranferred */
    for (size_t i = 0; i < mLayers.size(); i++) {
        mFenceTracer.setFenceInfo(mLayers[i]->mReleaseFence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_SRC_RELEASE, FENCE_IP_LAYER, FENCE_TO);
    }

    doPostProcessing();
    clearWinConfigData();

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::disableReadback() {
    setReadbackBufferInternal(nullptr, -1);
    mDpuData.enable_readback = false;
    return HWC2_ERROR_NONE;
}

bool ExynosDisplay::needChangeConfig(hwc2_config_t config) {
    if ((mDisplayConfigs.size() == 1) && (config == 0)) {
        ALOGI("%s, single config : %d", __func__, config);
        return false;
    }

    /* getting current config and compare */
    /* If same value, return */
    if ((mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_PENDING) ||
        (mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_REQUESTED)) {
        if (config == mDesiredConfig) {
            ALOGI("%s, desired config is same with requested config : (desried : %d, requested : %d)",
                  __func__, mDesiredConfig, config);
            return false;
        }
        ALOGI("%s, new config is set before previous config is applied(state: %d, desried : %d, requested: %d)",
              __func__, mConfigRequestState, mDesiredConfig, config);
    } else if ((mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_NONE) &&
               (mActiveConfig == config)) {
        ALOGI("%s, Same config change requested : %d", __func__, config);
        return false;
    }

    return true;
}

int32_t ExynosDisplay::setActiveConfig(hwc2_config_t config) {
    Mutex::Autolock lock(mDisplayMutex);

    if (isBadConfig(config))
        return HWC2_ERROR_BAD_CONFIG;

    DISPLAY_LOGD(eDebugDisplayConfig, "%s : %dx%d, %dms, %d Xdpi, %d Ydpi, powerMode(%d)", __func__,
                 mXres, mYres, mVsyncPeriod, mXdpi, mYdpi, mPowerModeState);

    if (mPowerModeState != HWC2_POWER_MODE_ON) {
        mPendConfigInfo.setPendConfig(config);
        return HWC2_ERROR_NONE;
    }

    if (!needChangeConfig(config))
        return HWC2_ERROR_NONE;

    DISPLAY_LOGD(eDebugDisplayConfig, "%s : %d(%dx%d), %dms", __func__,
                 config, mDisplayConfigs[config].width, mDisplayConfigs[config].height,
                 mDisplayConfigs[config].vsyncPeriod);

    return setActiveConfigInternal(config);
}

int32_t ExynosDisplay::setClientTarget(buffer_handle_t target,
                                       int32_t acquireFence, int32_t dataspace, uint64_t &geometryFlag) {
    buffer_handle_t handle = NULL;
    if (target != NULL)
        handle = target;

    ExynosGraphicBufferMeta gmeta(handle);
    if (mClientCompositionInfo.mHasCompositionLayer == false) {
        if (acquireFence >= 0)
            mFenceTracer.fence_close(acquireFence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB,
                                     "display::setClientTarget: acquireFence");
    } else {
#ifdef DISABLE_FENCE
        if (acquireFence >= 0)
            mFenceTracer.fence_close(acquireFence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB);
        acquireFence = -1;
#endif
        acquireFence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB, acquireFence);
        if (handle == NULL) {
            DISPLAY_LOGD(eDebugOverlaySupported, "ClientTarget is NULL, skipStaic (%d)",
                         mClientCompositionInfo.mSkipFlag);
            if (mClientCompositionInfo.mSkipFlag == false) {
                DISPLAY_LOGE("ClientTarget is NULL");
                DISPLAY_LOGE("\t%s:: mRenderingState(%d)", __func__, mRenderingState);
            }
        } else {
            DISPLAY_LOGD(eDebugOverlaySupported, "ClientTarget handle: %p [fd: %d, %d, %d]",
                         handle, gmeta.fd, gmeta.fd1, gmeta.fd2);
            if ((mClientCompositionInfo.mSkipFlag == true) &&
                ((mClientCompositionInfo.mLastWinConfigData.fd_idma[0] != gmeta.fd) ||
                 (mClientCompositionInfo.mLastWinConfigData.fd_idma[1] != gmeta.fd1) ||
                 (mClientCompositionInfo.mLastWinConfigData.fd_idma[2] != gmeta.fd2))) {
                String8 errString;
                DISPLAY_LOGE("skip flag is enabled but buffer is updated lastConfig[%d, %d, %d], handle[%d, %d, %d]\n",
                             mClientCompositionInfo.mLastWinConfigData.fd_idma[0],
                             mClientCompositionInfo.mLastWinConfigData.fd_idma[1],
                             mClientCompositionInfo.mLastWinConfigData.fd_idma[2],
                             gmeta.fd, gmeta.fd1, gmeta.fd2);
                DISPLAY_LOGE("last win config");
                for (size_t i = 0; i < mLastDpuData.configs.size(); i++) {
                    errString.appendFormat("config[%zu]\n", i);
                    dumpConfig(errString, mLastDpuData.configs[i]);
                    DISPLAY_LOGE("\t%s", errString.c_str());
                    errString.clear();
                }
                errString.appendFormat("%s:: skip flag is enabled but buffer is updated\n",
                                       __func__);
                printDebugInfos(errString);
            }
        }
        if (mClientCompositionInfo.mDataSpace != static_cast<android_dataspace>(dataspace))
            setGeometryChanged(GEOMETRY_DISPLAY_DATASPACE_CHANGED, geometryFlag);
        if (mClientCompositionInfo.setTargetBuffer(handle, acquireFence, (android_dataspace)dataspace))
            mDisplayInterface->onClientTargetDestroyed(&mClientCompositionInfo);

        mFenceTracer.setFenceInfo(acquireFence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_SRC_RELEASE, FENCE_IP_FB, FENCE_FROM);
    }
    if (handle) {
        mClientCompositionInfo.mCompressionInfo = getCompressionInfo(handle);
        mClientCompositionInfo.mFormat = ExynosFormat(gmeta.format, mClientCompositionInfo.mCompressionInfo.type);
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setColorTransform(
    const float *matrix,
    int32_t /*android_color_transform_t*/ hint,
    uint64_t &geometryFlag) {
    if ((hint < HAL_COLOR_TRANSFORM_IDENTITY) ||
        (hint > HAL_COLOR_TRANSFORM_CORRECT_TRITANOPIA))
        return HWC2_ERROR_BAD_PARAMETER;
    ALOGI("%s:: %d, %d", __func__, mColorTransformHint, hint);
    setGeometryChanged(GEOMETRY_DISPLAY_COLOR_TRANSFORM_CHANGED, geometryFlag);
    mColorTransformHint = hint;
#ifdef HWC_SUPPORT_COLOR_TRANSFORM
    int fd = -1;

#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDisplayColorInterface &&
        (mDisplayColorInterface->setColorTransform(matrix, hint) == NO_ERROR)) {
        mDisplayColorInterface->getDqeLut(mDisplayColorCoefAddr);
        fd = mDisplayColorFd;
    }
#endif
    int ret = mDisplayInterface->setColorTransform(matrix, hint, fd);
    if (ret != HWC2_ERROR_NONE) {
        if ((ret == HWC2_ERROR_UNSUPPORTED) &&
            (hint == HAL_COLOR_TRANSFORM_IDENTITY))
            return HWC2_ERROR_NONE;
        // invalid hint setting for use client composition
        mColorTransformHint = -EINVAL;
        setGeometryChanged(GEOMETRY_DISPLAY_COLOR_TRANSFORM_CHANGED, geometryFlag);
    }
    return ret;
#else
    return HWC2_ERROR_NONE;
#endif
}

#ifdef USE_DISPLAY_COLOR_INTERFACE
int32_t ExynosDisplay::searchDisplayColorMode(int32_t mode) {
    if (mDisplayColorModes.find(mode) != mDisplayColorModes.end()) {
        if (mDisplayColorInterface &&
            mDisplayColorInterface->getDqeLut(mDisplayColorCoefAddr) != NO_ERROR) {
            ALOGI("%s:: getting DqeLut fail", __func__);
            return -EINVAL;
        } else {
            return NO_ERROR;
        }
    } else {
        return -EINVAL;
    }
}

int32_t ExynosDisplay::searchDisplayRenderIntent(int32_t mode, int32_t intent) {
    for (auto item : mDisplayRenderIntents[mode]) {
        if (item.intentId == intent)
            return NO_ERROR;
    }
    return -EINVAL;
}
#endif
int32_t ExynosDisplay::setColorMode(
    int32_t /*android_color_mode_t*/ mode, bool canProcessWCG,
    uint64_t &geometryFlag) {
    if ((canProcessWCG == false) && (mode != HAL_COLOR_MODE_NATIVE))
        return HWC2_ERROR_UNSUPPORTED;

    int32_t curMode = mode;
    int32_t fd = -1;

#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDisplayColorInterface &&
        (mDisplayColorInterface->setColorMode(mode) == NO_ERROR) &&
        (searchDisplayColorMode(mode) == NO_ERROR)) {
        fd = mDisplayColorFd;
        if (mCurrentDisplayColorMode.modeId != mode)
            setGeometryChanged(GEOMETRY_DISPLAY_COLOR_MODE_CHANGED, geometryFlag);
        mCurrentDisplayColorMode = mDisplayColorModes[mode];
        curMode = mCurrentDisplayColorMode.gamutId;
    }
#endif

    if (mDisplayInterface->setColorMode(curMode, fd) < 0) {
        if (mode == HAL_COLOR_MODE_NATIVE)
            return HWC2_ERROR_NONE;

        DISPLAY_LOGE("%s:: is not supported", __func__);
        return HWC2_ERROR_UNSUPPORTED;
    }

    ALOGI("%s:: %d, %d", __func__, mColorMode, curMode);
    if (mColorMode != curMode)
        setGeometryChanged(GEOMETRY_DISPLAY_COLOR_MODE_CHANGED, geometryFlag);
    mColorMode = (android_color_mode_t)curMode;

    if (mHdrCoefInterface) {
        if (mColorMode == HAL_COLOR_MODE_NATIVE)
            mHdrTargetInfo.dataspace = HAL_DATASPACE_V0_SRGB;
        else
            mHdrTargetInfo.dataspace = colorModeToDataspace(mColorMode);
        mHdrCoefInterface->setTargetInfo(&mHdrTargetInfo);
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getRenderIntents(int32_t mode, uint32_t *outNumIntents,
                                        int32_t * /*android_render_intent_v1_1_t*/ outIntents) {
    ALOGI("%s:: mode(%d), outNum(%d), outIntents(%p)",
          __func__, mode, *outNumIntents, outIntents);
#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDisplayColorInterface) {
        if (outIntents == nullptr) {
            if (mDisplayRenderIntents.size() == 0)
                mDisplayRenderIntents.clear();
            if (mDisplayRenderIntents[mode].size() == 0)
                mDisplayRenderIntents[mode].clear();
            std::vector<DisplayRenderIntent> list = mDisplayColorInterface->getRenderIntents(mode);
            mDisplayRenderIntents[mode] = list;
            *outNumIntents = list.size();
        } else {
            for (int32_t i = 0; i < *outNumIntents; i++) {
                DisplayRenderIntent cur = mDisplayRenderIntents[mode].at(i);
                outIntents[i] = cur.intentId;
                ALOGI("%s:: Supported renderIntents [%u] (%s)", __func__,
                      cur.intentId, cur.intentName.c_str());
            }
        }
        return HWC2_ERROR_NONE;
    }
#endif

    return mDisplayInterface->getRenderIntents(mode, outNumIntents, outIntents);
}

int32_t ExynosDisplay::setColorModeWithRenderIntent(
    int32_t /*android_color_mode_t*/ mode,
    int32_t /*android_render_intent_v1_1_t */ intent, bool canProcessWCG,
    uint64_t &geometryFlag) {
    ALOGI("%s:: mode(%d), intent(%d)", __func__, mode, intent);
    int32_t curMode = mode;
    int32_t fd = -1;
    if ((canProcessWCG == false) && (mode != HAL_COLOR_MODE_NATIVE))
        return HWC2_ERROR_UNSUPPORTED;

#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDisplayColorInterface && (searchDisplayRenderIntent(mode, intent) == NO_ERROR) &&
        (mDisplayColorInterface->setColorModeWithRenderIntent(mode, intent) == NO_ERROR) &&
        (searchDisplayColorMode(mode) == NO_ERROR)) {
        fd = mDisplayColorFd;
        if (mCurrentDisplayColorMode.modeId != mode)
            setGeometryChanged(GEOMETRY_DISPLAY_COLOR_MODE_CHANGED, geometryFlag);
        mCurrentDisplayColorMode = mDisplayColorModes[mode];
        curMode = mCurrentDisplayColorMode.gamutId;
    }
#endif

    if (mDisplayInterface->setColorModeWithRenderIntent(curMode, intent, fd) < 0)
        return HWC2_ERROR_UNSUPPORTED;
    else {
        if (mColorMode != curMode)
            setGeometryChanged(GEOMETRY_DISPLAY_COLOR_MODE_CHANGED,
                               geometryFlag);
        mColorMode = (android_color_mode_t)curMode;

        if (mHdrCoefInterface) {
            if (mColorMode == HAL_COLOR_MODE_NATIVE)
                mHdrTargetInfo.dataspace = HAL_DATASPACE_V0_SRGB;
            else
                mHdrTargetInfo.dataspace = colorModeToDataspace(mColorMode);
            mHdrCoefInterface->setTargetInfo(&mHdrTargetInfo);
            mHdrCoefInterface->setRenderIntent(intent);
        }

        return HWC2_ERROR_NONE;
    }
}

int32_t ExynosDisplay::getDisplayIdentificationData(uint8_t *outPort,
                                                    uint32_t *outDataSize, uint8_t *outData) {
    return mDisplayInterface->getDisplayIdentificationData(outPort, outDataSize, outData);
}

int32_t ExynosDisplay::getDisplayCapabilities(uint32_t *outNumCapabilities,
                                              uint32_t *outCapabilities) {
    /* If each display has their own capabilities,
     * this should be described in display module codes */

    uint32_t capabilityNum = 0;

    if (mBrightnessOfs.is_open())
        capabilityNum++;

    if (mDisplayInterface->isDozeModeAvailable()) {
        capabilityNum++;
    }
#ifdef HWC_SUPPORT_COLOR_TRANSFORM
#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mType == HWC_DISPLAY_PRIMARY)
#endif
        capabilityNum++;
#endif

    if (outCapabilities == NULL) {
        *outNumCapabilities = capabilityNum;
        return HWC2_ERROR_NONE;
    }
    if (capabilityNum != *outNumCapabilities) {
        DISPLAY_LOGE("%s:: invalid outNumCapabilities(%d), should be(%d)", __func__, *outNumCapabilities, capabilityNum);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    uint32_t index = 0;

    if (mBrightnessOfs.is_open())
        outCapabilities[index++] = HWC2_DISPLAY_CAPABILITY_BRIGHTNESS;

    if (mDisplayInterface->isDozeModeAvailable()) {
        ALOGD("%s, Doze enabed", __func__);
        outCapabilities[index++] = HWC2_DISPLAY_CAPABILITY_DOZE;
    }
#ifdef HWC_SUPPORT_COLOR_TRANSFORM
#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mType == HWC_DISPLAY_PRIMARY)
#endif
        outCapabilities[index++] = HWC2_DISPLAY_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM;
#endif

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getDisplayBrightnessSupport(bool *outSupport) {
    if (!mBrightnessOfs.is_open()) {
        *outSupport = false;
    } else {
        *outSupport = true;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setDisplayBrightness(float brightness) {
    if (!mBrightnessOfs.is_open())
        return HWC2_ERROR_UNSUPPORTED;

    uint32_t scaledBrightness = static_cast<uint32_t>(round(brightness * mMaxBrightness));

#ifdef MINIMUM_DISPLAY_BRIGHTNESS
    // The special value of -1.0 is only passed when display is turning off
    // so it should be allowed to set a lower than minimum brightness.
    if (brightness != -1.0) {
        scaledBrightness = std::max(scaledBrightness, (uint32_t)MINIMUM_DISPLAY_BRIGHTNESS);
    }
#endif

    mBrightnessOfs.seekp(std::ios_base::beg);
    mBrightnessOfs << std::to_string(scaledBrightness);
    mBrightnessOfs.flush();
    if (mBrightnessOfs.fail()) {
        DISPLAY_LOGE("brightness write failed");
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getDisplayVsyncPeriod(hwc2_vsync_period_t *outVsyncPeriod) {
    Mutex::Autolock lock(mDisplayMutex);
    return getDisplayVsyncPeriodInternal(outVsyncPeriod);
}

int32_t ExynosDisplay::getDisplayVsyncTimestamp(uint64_t *outVsyncTimestamp) {
    int32_t ret = mDisplayInterface->getDisplayVsyncTimestamp(outVsyncTimestamp);
    if (ret != HWC2_ERROR_NONE)
        *outVsyncTimestamp = 0;

    return ret;
}

int32_t ExynosDisplay::getConfigAppliedTime(const uint64_t desiredTime,
                                            const uint64_t actualChangeTime,
                                            int64_t &appliedTime, int64_t &refreshTime) {
    uint32_t transientDuration = mDisplayInterface->getConfigChangeDuration();
    appliedTime = actualChangeTime;
    while (desiredTime > appliedTime) {
        DISPLAY_LOGD(eDebugDisplayConfig, "desired time(%" PRId64 ") > applied time(%" PRId64 ")", desiredTime, appliedTime);
        ;
        appliedTime += mVsyncPeriod;
    }

    refreshTime = appliedTime - (transientDuration * mVsyncPeriod);

    return NO_ERROR;
}

int32_t ExynosDisplay::checkValidationConfigConstraints(hwc2_config_t config,
                                                        hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
                                                        hwc_vsync_period_change_timeline_t *outTimeline) {
    if (isBadConfig(config))
        return HWC2_ERROR_BAD_CONFIG;

    if (mDisplayConfigs[mActiveConfig].groupId != mDisplayConfigs[config].groupId) {
        if (vsyncPeriodChangeConstraints->seamlessRequired) {
            DISPLAY_LOGE("%s:: Seamless is not allowed", __func__);
            return HWC2_ERROR_SEAMLESS_NOT_ALLOWED;
        }
        if (vsyncPeriodChangeConstraints->desiredTimeNanos > systemTime(SYSTEM_TIME_MONOTONIC)) {
            DISPLAY_LOGE("%s:: desired time is not supported", __func__);
            return HWC2_ERROR_UNSUPPORTED;
        }
    }

    if (vsyncPeriodChangeConstraints->seamlessRequired) {
        displayConfigs_t displayConfig = mDisplayConfigs[config];
        if ((mDisplayInterface->setActiveConfigWithConstraints(config, displayConfig, true)) != NO_ERROR) {
            DISPLAY_LOGE("%s:: Seamless is not possible", __func__);
            return HWC2_ERROR_SEAMLESS_NOT_POSSIBLE;
        }
    }

    return NO_ERROR;
}

int32_t ExynosDisplay::setActiveConfigWithConstraints(hwc2_config_t config,
                                                      hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
                                                      hwc_vsync_period_change_timeline_t *outTimeline,
                                                      bool needUpdateTimeline) {
    Mutex::Autolock lock(mDisplayMutex);

    ATRACE_CALL();

    int32_t ret = checkValidationConfigConstraints(config,
                                                   vsyncPeriodChangeConstraints,
                                                   outTimeline);
    if (ret != NO_ERROR)
        return ret;

    DISPLAY_LOGD(eDebugDisplayConfig, "config(%d), seamless(%d), desiredTime(%" PRId64 "), powerMode(%d)",
                 config, vsyncPeriodChangeConstraints->seamlessRequired,
                 vsyncPeriodChangeConstraints->desiredTimeNanos, mPowerModeState);

    if (mPowerModeState != HWC2_POWER_MODE_ON) {
        outTimeline->refreshRequired = true;
        getConfigAppliedTime(vsyncPeriodChangeConstraints->desiredTimeNanos,
                             vsyncPeriodChangeConstraints->desiredTimeNanos,
                             outTimeline->newVsyncAppliedTimeNanos,
                             outTimeline->refreshTimeNanos);
        mPendConfigInfo.setPendConfigWithConstraints(config, *vsyncPeriodChangeConstraints, *outTimeline);
        return HWC2_ERROR_NONE;
    }

    if (!needChangeConfig(config)) {
        if (needUpdateTimeline) {
            outTimeline->refreshRequired = false;
            outTimeline->newVsyncAppliedTimeNanos = vsyncPeriodChangeConstraints->desiredTimeNanos;
        }
        return HWC2_ERROR_NONE;
    }

    ATRACE_FORMAT("%dx%d -> %dx%d, %dms -> %dms", mXres, mYres,
                  mDisplayConfigs[config].width, mDisplayConfigs[config].height,
                  mVsyncPeriod, mDisplayConfigs[config].vsyncPeriod);
    DISPLAY_LOGD(eDebugDisplayConfig, "%dx%d, %dms, %d Xdpi, %d Ydpi",
                 mXres, mYres, mVsyncPeriod, mXdpi, mYdpi);

    int64_t actualChangeTime = 0;
    /* actualChangeTime includes transient duration */
    if (mDisplayInterface->getVsyncAppliedTime(config, mDisplayConfigs[config], &actualChangeTime) != NO_ERROR)
        return HWC2_ERROR_UNSUPPORTED;

    mConfigRequestState = hwc_request_state_t::SET_CONFIG_STATE_PENDING;
    mVsyncPeriodChangeConstraints = *vsyncPeriodChangeConstraints;
    mDesiredConfig = config;

    DISPLAY_LOGD(eDebugDisplayConfig, "requested config : %d(%dx%d/%d)->%d(%dx%d/%d), "
                                      "desired %" PRId64 ", newVsyncAppliedTimeNanos : %" PRId64 "",
                 mActiveConfig,
                 mDisplayConfigs[mActiveConfig].width, mDisplayConfigs[mActiveConfig].height,
                 mDisplayConfigs[mActiveConfig].vsyncPeriod,
                 config, mDisplayConfigs[config].width, mDisplayConfigs[config].height,
                 mDisplayConfigs[config].vsyncPeriod,
                 mVsyncPeriodChangeConstraints.desiredTimeNanos,
                 outTimeline->newVsyncAppliedTimeNanos);

    if (mDisplayConfigs[mActiveConfig].groupId != mDisplayConfigs[config].groupId) {
        setActiveConfigInternal(config);
        outTimeline->newVsyncAppliedTimeNanos = systemTime(SYSTEM_TIME_MONOTONIC);
        outTimeline->refreshRequired = false;
        return HWC2_ERROR_NONE;
    }

    if (needUpdateTimeline) {
        outTimeline->refreshRequired = true;
        getConfigAppliedTime(mVsyncPeriodChangeConstraints.desiredTimeNanos,
                             actualChangeTime,
                             outTimeline->newVsyncAppliedTimeNanos,
                             outTimeline->refreshTimeNanos);
    }

    mVsyncAppliedTimeLine = *outTimeline;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setBootDisplayConfig(int32_t config) {
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplay::clearBootDisplayConfig() {
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplay::getPreferredBootDisplayConfig(int32_t *outConfig) {
    return getPreferredDisplayConfigInternal(outConfig);
}

int32_t ExynosDisplay::getPreferredDisplayConfigInternal(int32_t *outConfig) {
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplay::setAutoLowLatencyMode(bool __unused on) {
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplay::getSupportedContentTypes(uint32_t *__unused outNumSupportedContentTypes,
                                                uint32_t *__unused outSupportedContentTypes) {
    if (outSupportedContentTypes == NULL)
        outNumSupportedContentTypes = 0;
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::getClientTargetProperty(hwc_client_target_property_t *outClientTargetProperty) {
    outClientTargetProperty->pixelFormat = HAL_PIXEL_FORMAT_RGBA_8888;
    outClientTargetProperty->dataspace = HAL_DATASPACE_UNKNOWN;
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setActiveConfigInternal(hwc2_config_t config) {
    int32_t ret = HWC2_ERROR_NONE;

    DISPLAY_LOGD(eDebugDisplayConfig, "(current %d) : %dx%d, %dms, %d Xdpi, %d Ydpi", mActiveConfig,
                 mXres, mYres, mVsyncPeriod, mXdpi, mYdpi);
    DISPLAY_LOGD(eDebugDisplayConfig, "(requested %d) : %dx%d, %dms, %d Xdpi, %d Ydpi", config,
                 mDisplayConfigs[config].width, mDisplayConfigs[config].height, mDisplayConfigs[config].vsyncPeriod,
                 mDisplayConfigs[config].Xdpi, mDisplayConfigs[config].Ydpi);

    mConfigChangeTimoutCnt = 0;

    displayConfigs_t displayConfig = mDisplayConfigs[config];
    if (mDisplayInterface->setActiveConfig(config, displayConfig) < 0) {
        DISPLAY_LOGE("%s bad config request", __func__);
        return HWC2_ERROR_BAD_CONFIG;
    }
    /* Working vsync period can be chaged when active config is changed */
    mWorkingVsyncInfo.setVsyncPeriod(static_cast<uint32_t>(mDisplayInterface->getWorkingVsyncPeriod()));

    /*
     * resetConfigRequestState calls fpsChangedCallback
     * Callback checks mActiveConfig so keep the call sequence
     * updateInternalDisplayConfigVariables() first
     * resetConfigRequestState
     */
    updateInternalDisplayConfigVariables(config, false);
    /*
     * mDesiredConfig should be set
     * resetConfigRequestState uses mDesiredConfig
     */
    mDesiredConfig = config;
    resetConfigRequestState();

    if (mPowerModeState == HWC_POWER_MODE_OFF)
        mDisplayConfigPending = true;
    else
        mDisplayConfigPending = false;

    return ret;
}

bool ExynosDisplay::isBadConfig(hwc2_config_t config) {
    /* Check invalid config */
    const auto its = mDisplayConfigs.find(config);
    if (its == mDisplayConfigs.end()) {
        DISPLAY_LOGE("%s, invalid config : %d", __func__, config);
        return true;
    }

    return false;
}

int32_t ExynosDisplay::updateInternalDisplayConfigVariables(hwc2_config_t config, bool updateVsync) {
    mActiveConfig = config;

    /* Update internal variables */
    auto it = mDisplayConfigs.find(config);
    if (it == mDisplayConfigs.end()) {
        ALOGE("%s:: Can't find config(%d)", __func__, config);
        return HWC2_ERROR_BAD_CONFIG;
    }
    mXres = it->second.width;
    mYres = it->second.height;
    mXdpi = it->second.Xdpi;
    mYdpi = it->second.Ydpi;

    DISPLAY_LOGD(eDebugDisplayConfig, "%s(update %d) : %dx%d, %dms, %d Xdpi, %d Ydpi",
                 __func__, mActiveConfig, mXres, mYres, mVsyncPeriod, mXdpi, mYdpi);

    if (updateVsync)
        mVsyncPeriod = it->second.vsyncPeriod;

    return NO_ERROR;
}

int32_t ExynosDisplay::resetConfigRequestState() {
    getDisplayAttribute(mDesiredConfig, HWC2_ATTRIBUTE_VSYNC_PERIOD,
                        (int32_t *)&mVsyncPeriod);
    DISPLAY_LOGD(eDebugDisplayConfig, "Update mVsyncPeriod %d",
                 mVsyncPeriod);
    if (mFpsChangedCallback)
        mFpsChangedCallback->fpsChangedCallback();
    mConfigRequestState = hwc_request_state_t::SET_CONFIG_STATE_NONE;
    mDisplayInterface->resetConfigRequestState();
    return NO_ERROR;
}

int32_t ExynosDisplay::updateConfigRequestAppliedTime() {
    if (mConfigRequestState != hwc_request_state_t::SET_CONFIG_STATE_REQUESTED)
        return NO_ERROR;
    /*
     * config change was requested but
     * it is not applied until newVsyncAppliedTimeNanos
     * Update time information
     */
    int64_t actualChangeTime = 0;
    int32_t ret = mDisplayInterface->getVsyncAppliedTime(mDesiredConfig,
                                                         mDisplayConfigs[mDesiredConfig], &actualChangeTime);
    if (ret != NO_ERROR)
        return ret;
    return updateVsyncAppliedTimeLine(actualChangeTime);
}

int32_t ExynosDisplay::updateVsyncAppliedTimeLine(int64_t actualChangeTime) {
    hwc2_callback_data_t vsync_callbackData = nullptr;
    HWC2_PFN_VSYNC_PERIOD_TIMING_CHANGED vsync_callbackFunc = nullptr;
    if (mCallbackInfos[HWC2_CALLBACK_VSYNC_PERIOD_TIMING_CHANGED].funcPointer != NULL) {
        vsync_callbackData =
            mCallbackInfos[HWC2_CALLBACK_VSYNC_PERIOD_TIMING_CHANGED].callbackData;
        vsync_callbackFunc =
            (HWC2_PFN_VSYNC_PERIOD_TIMING_CHANGED)mCallbackInfos[HWC2_CALLBACK_VSYNC_PERIOD_TIMING_CHANGED].funcPointer;
    }

    DISPLAY_LOGD(eDebugDisplayConfig, "Vsync applied time is changed (%" PRId64 "-> %" PRId64 ")",
                 mVsyncAppliedTimeLine.newVsyncAppliedTimeNanos,
                 actualChangeTime);
    getConfigAppliedTime(mVsyncPeriodChangeConstraints.desiredTimeNanos,
                         actualChangeTime,
                         mVsyncAppliedTimeLine.newVsyncAppliedTimeNanos,
                         mVsyncAppliedTimeLine.refreshTimeNanos);
    if (mConfigRequestState ==
        hwc_request_state_t::SET_CONFIG_STATE_REQUESTED) {
        mVsyncAppliedTimeLine.refreshRequired = false;
    } else {
        mVsyncAppliedTimeLine.refreshRequired = true;
    }

    DISPLAY_LOGD(eDebugDisplayConfig, "refresh required(%d), newVsyncAppliedTimeNanos (%" PRId64 ")",
                 mVsyncAppliedTimeLine.refreshRequired,
                 mVsyncAppliedTimeLine.newVsyncAppliedTimeNanos);

    /* TODO check display Id */
    if (vsync_callbackFunc != nullptr) {
        vsync_callbackFunc(vsync_callbackData, getDisplayId(HWC_DISPLAY_PRIMARY, 0),
                           &mVsyncAppliedTimeLine);
    } else {
        hwc2_vsync_period_t period;
        mDisplayInterface->getDisplayVsyncPeriod(&period);
        ALOGD("callback function is null, reqState(%d), active(%d), desired(%d), vsyncEnState(%d, %d), vsyncPeriod(%d, %" PRId64 ")",
              mConfigRequestState, mActiveConfig, mDesiredConfig, mVsyncState,
              mVsyncCallback.getVSyncEnabled(), period, mVsyncCallback.getDesiredVsyncPeriod());
    }

    return NO_ERROR;
}

int32_t ExynosDisplay::getDisplayVsyncPeriodInternal(hwc2_vsync_period_t *outVsyncPeriod) {
    /* Getting actual config from DPU */
    if (mDisplayInterface->getDisplayVsyncPeriod(outVsyncPeriod) == HWC2_ERROR_NONE) {
        DISPLAY_LOGD(eDebugDisplayInterfaceConfig, "period : %ld (time : %" PRId64 ")",
                     (long)*outVsyncPeriod, systemTime(SYSTEM_TIME_MONOTONIC));
    } else {
        *outVsyncPeriod = mVsyncPeriod;
        DISPLAY_LOGD(eDebugDisplayInterfaceConfig, "period is mVsyncPeriod: %d",
                     mVsyncPeriod);
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::doDisplayConfigPostProcess() {
    uint64_t current = systemTime(SYSTEM_TIME_MONOTONIC);

    int64_t actualChangeTime = 0;
    int32_t ret = mDisplayInterface->getVsyncAppliedTime(mDesiredConfig,
                                                         mDisplayConfigs[mDesiredConfig], &actualChangeTime);
    if (ret != NO_ERROR)
        return ret;

    if (mPowerModeState != HWC2_POWER_MODE_ON) {
        return NO_ERROR;
    }

    bool needSetActiveConfig = false;

    DISPLAY_LOGD(eDebugDisplayConfig,
                 "Check time for setActiveConfig (curr: %" PRId64
                 ", actualChangeTime: %" PRId64 ", desiredTime: %" PRId64 "",
                 current, actualChangeTime,
                 mVsyncPeriodChangeConstraints.desiredTimeNanos);
    if (actualChangeTime >= mVsyncPeriodChangeConstraints.desiredTimeNanos) {
        DISPLAY_LOGD(eDebugDisplayConfig, "Request setActiveConfig");
        needSetActiveConfig = true;
    } else {
        DISPLAY_LOGD(eDebugDisplayConfig, "setActiveConfig still pending");
    }

    if (needSetActiveConfig) {
        displayConfigs_t displayConfig = mDisplayConfigs[mDesiredConfig];
        if ((ret = mDisplayInterface->setActiveConfigWithConstraints(mDesiredConfig, displayConfig)) != NO_ERROR)
            return ret;

        int32_t desiredPeriod = 0;
        getDisplayAttribute(mDesiredConfig, HWC2_ATTRIBUTE_VSYNC_PERIOD,
                            &desiredPeriod);
        mVsyncCallback.setDesiredVsyncPeriod(desiredPeriod);
        /* Enable vsync to check vsync period */
        setVsyncEnabledInternal(HWC2_VSYNC_ENABLE);

        mConfigRequestState = hwc_request_state_t::SET_CONFIG_STATE_REQUESTED;
    }

    return updateVsyncAppliedTimeLine(actualChangeTime);
}

int32_t ExynosDisplay::getDisplayConnectionType(uint32_t *outType) {
    if (mType == HWC_DISPLAY_PRIMARY)
        *outType = HWC2_DISPLAY_CONNECTION_TYPE_INTERNAL;
    else if (mType == HWC_DISPLAY_EXTERNAL)
        *outType = HWC2_DISPLAY_CONNECTION_TYPE_EXTERNAL;
    else
        return HWC2_ERROR_BAD_DISPLAY;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setContentType(int32_t /* hwc2_content_type_t */ contentType) {
    if (contentType == HWC2_CONTENT_TYPE_NONE)
        return HWC2_ERROR_NONE;

    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplay::setOutputBuffer(buffer_handle_t __unused buffer, int32_t __unused releaseFence) {
    return HWC2_ERROR_NONE;
}

int ExynosDisplay::clearDisplay() {
    const int ret = mDisplayInterface->clearDisplay();
    if (ret)
        DISPLAY_LOGE("fail to clear display");

    mClientCompositionInfo.mSkipStaticInitFlag = false;
    mClientCompositionInfo.mSkipFlag = false;

    mLastDpuData.reset();

    /* Update last present fence */
    mLastPresentFence = mFenceTracer.fence_close(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                                 "display::clearDisplay: mLastPresentFence");

    return ret;
}

int32_t ExynosDisplay::setPowerMode(int32_t /*hwc2_power_mode_t*/ mode,
                                    uint64_t &geometryFlag) {
    if (!mDisplayInterface->isDozeModeAvailable() &&
        (mode == HWC2_POWER_MODE_DOZE || mode == HWC2_POWER_MODE_DOZE_SUSPEND)) {
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (mode == HWC_POWER_MODE_OFF) {
        clearDisplay();
        ALOGV("HWC2: Clear display (power off)");
    }

    /* TODO: Call display interface */
    mDisplayInterface->setPowerMode(mode);

    ALOGD("%s:: mode(%d))", __func__, mode);

    mPowerModeState = (hwc2_power_mode_t)mode;

    if (mode == HWC_POWER_MODE_OFF) {
        /* It should be called from validate() when the screen is on */
        mNeedSkipPresent = true;
        setGeometryChanged(GEOMETRY_DISPLAY_POWER_OFF, geometryFlag);
        if ((mRenderingState >= RENDERING_STATE_VALIDATED) &&
            (mRenderingState < RENDERING_STATE_PRESENTED))
            closeFencesForSkipFrame(RENDERING_STATE_VALIDATED);
        mRenderingState = RENDERING_STATE_NONE;
    } else {
        setGeometryChanged(GEOMETRY_DISPLAY_POWER_ON, geometryFlag);
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::setVsyncEnabled(
    int32_t /*hwc2_vsync_t*/ enabled) {
    Mutex::Autolock lock(mDisplayMutex);
    if (enabled == HWC2_VSYNC_ENABLE) {
        setVsyncEnabledInternal(enabled);
    } else {
        if (mVsyncCallback.getDesiredVsyncPeriod() == 0)
            setVsyncEnabledInternal(HWC2_VSYNC_DISABLE);
    }
    mVsyncCallback.enableVSync(HWC2_VSYNC_ENABLE == enabled);
    return NO_ERROR;
}

int32_t ExynosDisplay::setVsyncEnabledInternal(
    int32_t enabled) {
    __u32 val = 0;

    if (enabled < 0 || enabled > HWC2_VSYNC_DISABLE)
        return HWC2_ERROR_BAD_PARAMETER;

    if (enabled == HWC2_VSYNC_ENABLE) {
        gettimeofday(&updateTimeInfo.lastEnableVsyncTime, NULL);
        val = 1;
    } else {
        gettimeofday(&updateTimeInfo.lastDisableVsyncTime, NULL);
    }

    if (mDisplayInterface->setVsyncEnabled(val) < 0) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "vsync ioctl failed errno : %d", errno);
        return HWC_HAL_ERROR_INVAL;
    }

    mVsyncState = (hwc2_vsync_t)enabled;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::forceSkipValidateDisplay(
    uint32_t *outNumTypes, uint32_t *outNumRequests) {
    ATRACE_CALL();
    gettimeofday(&updateTimeInfo.lastValidateTime, NULL);
    mLastUpdateTimeStamp = systemTime(SYSTEM_TIME_MONOTONIC);

    HDEBUGLOGD(eDebugResourceManager,
               "%s validate is forced to be skipped",
               mDisplayName.c_str());
    *outNumTypes = 0;
    *outNumRequests = 0;

    mRenderingState = RENDERING_STATE_VALIDATED;

    /* Clear validateFlag */
    mRenderingStateFlags.validateFlag = false;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplay::preProcessValidate(DeviceValidateInfo &validateInfo,
                                          uint64_t &geometryChanged) {
    ATRACE_CALL();
    mLastUpdateTimeStamp = systemTime(SYSTEM_TIME_MONOTONIC);

    mLayers.vector_sort();
    doPreProcessing(validateInfo, geometryChanged);
    setSrcAcquireFences();
    setPerformanceSetting();

    return NO_ERROR;
}

void ExynosDisplay::setForceClient() {
    mClientCompositionInfo.mSkipStaticInitFlag = false;
    mExynosCompositionInfo.mSkipStaticInitFlag = false;
    mClientCompositionInfo.initializeInfos();
    mExynosCompositionInfo.initializeInfos();
    for (uint32_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->mOverlayInfo |= eResourceAssignFail;
        layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
        addClientCompositionLayer(i);
    }
}

int32_t ExynosDisplay::postProcessValidate() {
    ATRACE_CALL();
    int ret = NO_ERROR;

    if ((ret = updateColorConversionInfo()) != NO_ERROR) {
        DISPLAY_LOGE("%s:: updateColorConversionInfo() fail, ret(%d)",
                     __func__, ret);
        return ret;
    }

    if ((ret = skipStaticLayers(mClientCompositionInfo)) != NO_ERROR) {
        DISPLAY_LOGE("%s:: skipStaticLayers() fail, ret(%d)", __func__, ret);
    } else {
        if ((mClientCompositionInfo.mHasCompositionLayer) &&
            (mClientCompositionInfo.mSkipFlag == false)) {
            /* Initialize compositionType */
            for (size_t i = (size_t)mClientCompositionInfo.mFirstIndex; i <= (size_t)mClientCompositionInfo.mLastIndex; i++) {
                if (i >= mLayers.size())
                    break;
                if (mLayers[i]->mOverlayPriority >= ePriorityHigh)
                    continue;
                mLayers[i]->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            }
        }
    }

    if (mDisplayControl.earlyStartMPP == true) {
        if ((ret = startPostProcessing()) != NO_ERROR) {
            DISPLAY_LOGE("%s:: startPostProcessing() fail, ret(%d)",
                         __func__, ret);
            return ret;
        }
    }

    return ret;
}

int32_t ExynosDisplay::setValidateState(uint32_t &outNumTypes,
                                        uint32_t &outNumRequests, uint64_t &geometryChanged) {
    mRenderingState = RENDERING_STATE_VALIDATED;
    /*
     * isFirstValidate() should be checked only before setting validateFlag
     */
    mRenderingStateFlags.validateFlag = true;

    /* Validate is called so HWC don't need to skip present */
    mNeedSkipPresent = false;

    int ret = NO_ERROR;
    if ((ret = getChangedCompositionTypes(&outNumTypes, nullptr, nullptr)) != NO_ERROR) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: getChangedCompositionTypes() fail, display(%d), ret(%d)", __func__, mDisplayId, ret);
        setGeometryChanged(GEOMETRY_ERROR_CASE, geometryChanged);
        return ret;
    }
    if ((ret = getDisplayRequests(nullptr, &outNumRequests,
                                  nullptr, nullptr)) != NO_ERROR) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: getDisplayRequests() fail, display(%d), ret(%d)", __func__, mDisplayId, ret);
        setGeometryChanged(GEOMETRY_ERROR_CASE, geometryChanged);
        return ret;
    }

    if (outNumTypes == 0)
        return HWC2_ERROR_NONE;
    else
        return HWC2_ERROR_HAS_CHANGES;
}

int32_t ExynosDisplay::startPostProcessing() {
    int ret = NO_ERROR;
    String8 errString;

    auto handle_err = [=, this, &errString]() -> int32_t {
        printDebugInfos(errString);
        closeFences();
        mDisplayInterface->setForcePanic();
        return -EINVAL;
    };

    if ((ret = doExynosComposition()) != NO_ERROR) {
        errString.appendFormat("exynosComposition fail (%d)\n", ret);
        return handle_err();
    }

    // loop for all layer
    for (size_t i = 0; i < mLayers.size(); i++) {
        if ((mLayers[i]->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) &&
            (mLayers[i]->mM2mMPP != NULL)) {
            /* mAcquireFence is updated, Update image info */
            struct exynos_image srcImg, dstImg, midImg;
            mLayers[i]->setSrcExynosImage(&srcImg);
            mLayers[i]->setDstExynosImage(&dstImg);
            mLayers[i]->setExynosImage(srcImg, dstImg);
            ExynosMPP *m2mMpp = mLayers[i]->mM2mMPP;
            ExynosMPP *otfMpp = mLayers[i]->mOtfMPP;
            srcImg = mLayers[i]->mSrcImg;
            midImg = mLayers[i]->mMidImg;
            m2mMpp->requestHWStateChange(MPP_HW_STATE_RUNNING);

            /* VOTF setting before postprocessing */
            if (m2mMpp->canUseVotf(srcImg)) {
                VotfInfo votfInfo;
                otfMpp->enableVotfInfo(votfInfo);
                m2mMpp->setVotfInfo(votfInfo);
            }

            if ((ret = m2mMpp->doPostProcessing(srcImg, midImg)) != NO_ERROR) {
                DISPLAY_LOGE("%s:: doPostProcessing() failed, layer(%zu), ret(%d)",
                             __func__, i, ret);
                errString.appendFormat("%s:: doPostProcessing() failed, layer(%zu), ret(%d)\n",
                                       __func__, i, ret);
                return handle_err();
            } else {
                /* This should be closed by lib for each resource */
                mLayers[i]->mAcquireFence = -1;
            }
        }
    }
    return ret;
}

int32_t ExynosDisplay::setCursorPositionAsync(uint32_t x_pos, uint32_t y_pos) {
    mDisplayInterface->setCursorPositionAsync(x_pos, y_pos);
    return HWC2_ERROR_NONE;
}

void ExynosDisplay::dumpConfig(const exynos_win_config_data &c) {
    DISPLAY_LOGD(eDebugWinConfig | eDebugSkipStaicLayer, "\tstate = %u", c.state);
    if (c.state == c.WIN_STATE_COLOR) {
        DISPLAY_LOGD(eDebugWinConfig | eDebugSkipStaicLayer,
                     "\t\tx = %d, y = %d, width = %d, height = %d, color = %u, alpha = %f\n",
                     c.dst.x, c.dst.y, c.dst.w, c.dst.h, c.color, c.plane_alpha);
    } else /* if (c.state != c.WIN_STATE_DISABLED) */ {
        DISPLAY_LOGD(eDebugWinConfig | eDebugSkipStaicLayer, "\t\tfd = (%d, %d, %d), acq_fence = %d, rel_fence = %d "
                                                             "src_f_w = %u, src_f_h = %u, src_x = %d, src_y = %d, src_w = %u, src_h = %u, "
                                                             "dst_f_w = %u, dst_f_h = %u, dst_x = %d, dst_y = %d, dst_w = %u, dst_h = %u, "
                                                             "format = %s, pa = %f, transform = %d, dataspace = 0x%8x, hdr_enable = %d, blending = %u, "
                                                             "protection = %u, compressionType = %8x, compression_src = %d, transparent(x:%d, y:%d, w:%d, h:%d), "
                                                             "block(x:%d, y:%d, w:%d, h:%d)",
                     c.fd_idma[0], c.fd_idma[1], c.fd_idma[2],
                     c.acq_fence, c.rel_fence,
                     c.src.f_w, c.src.f_h, c.src.x, c.src.y, c.src.w, c.src.h,
                     c.dst.f_w, c.dst.f_h, c.dst.x, c.dst.y, c.dst.w, c.dst.h,
                     c.format.name().c_str(), c.plane_alpha, c.transform, c.dataspace, c.hdr_enable,
                     c.blending, c.protection, c.compressionInfo.type, c.comp_src,
                     c.transparent_area.x, c.transparent_area.y, c.transparent_area.w, c.transparent_area.h,
                     c.opaque_area.x, c.opaque_area.y, c.opaque_area.w, c.opaque_area.h);
    }
}

void ExynosDisplay::dump(String8 &result) {
    result.appendFormat("[%s] display information size: %d x %d, vsyncState: %d, colorMode: %d, colorTransformHint: %d\n",
                        mDisplayName.c_str(),
                        mXres, mYres, mVsyncState, mColorMode, mColorTransformHint);
    mClientCompositionInfo.dump(result);
    mExynosCompositionInfo.dump(result);

    for (uint32_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->dump(result);
    }
    result.appendFormat("\n");
}

void ExynosDisplay::dumpConfig(String8 &result, const exynos_win_config_data &c) {
    result.appendFormat("\tstate = %u\n", c.state);
    if (c.state == c.WIN_STATE_COLOR) {
        result.appendFormat("\t\tx = %d, y = %d, width = %d, height = %d, color = %u, alpha = %f\n",
                            c.dst.x, c.dst.y, c.dst.w, c.dst.h, c.color, c.plane_alpha);
    } else /* if (c.state != c.WIN_STATE_DISABLED) */ {
        result.appendFormat("\t\tfd = (%d, %d, %d), acq_fence = %d, rel_fence = %d "
                            "src_f_w = %u, src_f_h = %u, src_x = %d, src_y = %d, src_w = %u, src_h = %u, "
                            "dst_f_w = %u, dst_f_h = %u, dst_x = %d, dst_y = %d, dst_w = %u, dst_h = %u, "
                            "format = %s, pa = %f, transform = %d, dataspace = 0x%8x, hdr_enable = %d, blending = %u, "
                            "protection = %u, compressionType = %8x, compression_src = %d, transparent(x:%d, y:%d, w:%d, h:%d), "
                            "block(x:%d, y:%d, w:%d, h:%d)\n",
                            c.fd_idma[0], c.fd_idma[1], c.fd_idma[2],
                            c.acq_fence, c.rel_fence,
                            c.src.f_w, c.src.f_h, c.src.x, c.src.y, c.src.w, c.src.h,
                            c.dst.f_w, c.dst.f_h, c.dst.x, c.dst.y, c.dst.w, c.dst.h,
                            c.format.name().c_str(), c.plane_alpha, c.transform, c.dataspace, c.hdr_enable, c.blending, c.protection,
                            c.compressionInfo.type, c.comp_src,
                            c.transparent_area.x, c.transparent_area.y, c.transparent_area.w, c.transparent_area.h,
                            c.opaque_area.x, c.opaque_area.y, c.opaque_area.w, c.opaque_area.h);
    }
}

void ExynosDisplay::printConfig(exynos_win_config_data &c) {
    ALOGD("\tstate = %u", c.state);
    if (c.state == c.WIN_STATE_COLOR) {
        ALOGD("\t\tx = %d, y = %d, width = %d, height = %d, color = %u, alpha = %f\n",
              c.dst.x, c.dst.y, c.dst.w, c.dst.h, c.color, c.plane_alpha);
    } else /* if (c.state != c.WIN_STATE_DISABLED) */ {
        ALOGD("\t\tfd = (%d, %d, %d), acq_fence = %d, rel_fence = %d "
              "src_f_w = %u, src_f_h = %u, src_x = %d, src_y = %d, src_w = %u, src_h = %u, "
              "dst_f_w = %u, dst_f_h = %u, dst_x = %d, dst_y = %d, dst_w = %u, dst_h = %u, "
              "format = %s, pa = %f, transform = %d, dataspace = 0x%8x, hdr_enable = %d, blending = %u, "
              "protection = %u, compressionType = %8x, compression_src = %d, transparent(x:%d, y:%d, w:%d, h:%d), "
              "block(x:%d, y:%d, w:%d, h:%d)",
              c.fd_idma[0], c.fd_idma[1], c.fd_idma[2],
              c.acq_fence, c.rel_fence,
              c.src.f_w, c.src.f_h, c.src.x, c.src.y, c.src.w, c.src.h,
              c.dst.f_w, c.dst.f_h, c.dst.x, c.dst.y, c.dst.w, c.dst.h,
              c.format.name().c_str(), c.plane_alpha, c.transform, c.dataspace, c.hdr_enable, c.blending, c.protection,
              c.compressionInfo.type, c.comp_src,
              c.transparent_area.x, c.transparent_area.y, c.transparent_area.w, c.transparent_area.h,
              c.opaque_area.x, c.opaque_area.y, c.opaque_area.w, c.opaque_area.h);
    }
}

int32_t ExynosDisplay::setCompositionTargetExynosImage(uint32_t targetType, exynos_image *src_img, exynos_image *dst_img) {
    if ((targetType <= COMPOSITION_NONE) || (targetType >= COMPOSITION_MAX))
        return -EINVAL;

    auto setImgageFormCompositionInfo = [=, this](
                                            ExynosCompositionInfo &compositionInfo, exynos_image *src_img,
                                            exynos_image *dst_img) {
        if (compositionInfo.mTargetBuffer != NULL) {
            src_img->bufferHandle = compositionInfo.mTargetBuffer;
            src_img->exynosFormat = compositionInfo.mFormat;

#ifdef GRALLOC_VERSION1
            ExynosGraphicBufferMeta gmeta(compositionInfo.mTargetBuffer);
            src_img->usageFlags = gmeta.producer_usage;
#else
            src_img->usageFlags = compositionInfo.mTargetBuffer->flags;
#endif
        } else {
            src_img->bufferHandle = NULL;
            src_img->exynosFormat = PredefinedFormat::exynosFormatRgba8;
            src_img->usageFlags = 0;
        }
        src_img->acquireFenceFd = compositionInfo.mAcquireFence;
        src_img->dataSpace = compositionInfo.mDataSpace;
        src_img->compressionInfo = compositionInfo.mCompressionInfo;
        if ((targetType == COMPOSITION_CLIENT) && (!mUseDpu)) {
            if (compositionInfo.mLastIndex < mExynosCompositionInfo.mLastIndex)
                src_img->zOrder = 0;
            else
                src_img->zOrder = 1000;
        }
        dst_img->compressionInfo = compositionInfo.mCompressionInfo;
    };

    src_img->fullWidth = mXres;
    src_img->fullHeight = mYres;
    /* To do */
    /* Fb crop should be set hear */
    src_img->x = 0;
    src_img->y = 0;
    src_img->w = mXres;
    src_img->h = mYres;

    src_img->layerFlags = 0x0;
    src_img->releaseFenceFd = -1;
    src_img->blending = HWC2_BLEND_MODE_PREMULTIPLIED;
    src_img->transform = 0;
    src_img->planeAlpha = 1;
    src_img->zOrder = 0;

    dst_img->fullWidth = mXres;
    dst_img->fullHeight = mYres;
    /* To do */
    /* Fb crop should be set hear */
    dst_img->x = 0;
    dst_img->y = 0;
    dst_img->w = mXres;
    dst_img->h = mYres;

    dst_img->bufferHandle = NULL;
    dst_img->exynosFormat = PredefinedFormat::exynosFormatRgba8;
    dst_img->usageFlags = 0;

    dst_img->layerFlags = 0x0;
    dst_img->acquireFenceFd = -1;
    dst_img->releaseFenceFd = -1;
    dst_img->dataSpace = src_img->dataSpace;
    if (mColorMode != HAL_COLOR_MODE_NATIVE)
        dst_img->dataSpace = colorModeToDataspace(mColorMode);
    dst_img->blending = HWC2_BLEND_MODE_NONE;
    dst_img->transform = 0;
    dst_img->planeAlpha = 1;
    dst_img->zOrder = src_img->zOrder;

    if (targetType == COMPOSITION_CLIENT)
        setImgageFormCompositionInfo(mClientCompositionInfo, src_img, dst_img);
    else if (targetType == COMPOSITION_EXYNOS)
        setImgageFormCompositionInfo(mExynosCompositionInfo, src_img, dst_img);

    return NO_ERROR;
}

int32_t ExynosDisplay::initializeValidateInfos() {
    mCursorIndex = -1;
    for (uint32_t i = 0; i < mLayers.size(); i++) {
        ExynosLayer *layer = mLayers[i];
        layer->mValidateCompositionType = HWC2_COMPOSITION_INVALID;
        layer->mOverlayInfo = 0;
        if ((mDisplayControl.cursorSupport == true) &&
            (mLayers[i]->mCompositionType == HWC2_COMPOSITION_CURSOR))
            mCursorIndex = i;
    }

    exynos_image src_img;
    exynos_image dst_img;

    mClientCompositionInfo.initializeInfos();
    setCompositionTargetExynosImage(COMPOSITION_CLIENT, &src_img, &dst_img);
    mClientCompositionInfo.setExynosImage(src_img, dst_img);

    mExynosCompositionInfo.initializeInfos();
    setCompositionTargetExynosImage(COMPOSITION_EXYNOS, &src_img, &dst_img);
    mExynosCompositionInfo.setExynosImage(src_img, dst_img);

    return NO_ERROR;
}

int32_t ExynosDisplay::addClientCompositionLayer(uint32_t layerIndex,
                                                 uint32_t *isExynosCompositionChanged) {
    bool exynosCompositionChanged = false;
    int32_t ret = NO_ERROR;

    if (isExynosCompositionChanged != NULL)
        *isExynosCompositionChanged = 0;

    DISPLAY_LOGD(eDebugResourceManager, "[%d] layer is added to client composition", layerIndex);

    if (mClientCompositionInfo.mHasCompositionLayer == false) {
        mClientCompositionInfo.mFirstIndex = layerIndex;
        mClientCompositionInfo.mLastIndex = layerIndex;
        mClientCompositionInfo.mHasCompositionLayer = true;
        return EXYNOS_ERROR_CHANGED;
    } else {
        mClientCompositionInfo.mFirstIndex = min(mClientCompositionInfo.mFirstIndex, (int32_t)layerIndex);
        mClientCompositionInfo.mLastIndex = max(mClientCompositionInfo.mLastIndex, (int32_t)layerIndex);
    }
    DISPLAY_LOGD(eDebugResourceAssigning, "\tClient composition range [%d] - [%d]",
                 mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex);

    if ((mClientCompositionInfo.mFirstIndex < 0) || (mClientCompositionInfo.mLastIndex < 0)) {
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: mClientCompositionInfo.mHasCompositionLayer is true "
                                                 "but index is not valid (firstIndex: %d, lastIndex: %d)",
                 __func__, mClientCompositionInfo.mFirstIndex,
                 mClientCompositionInfo.mLastIndex);
        return -EINVAL;
    }

    /* handle sandwiched layers */
    for (uint32_t i = (uint32_t)mClientCompositionInfo.mFirstIndex + 1; i < (uint32_t)mClientCompositionInfo.mLastIndex; i++) {
        ExynosLayer *layer = mLayers[i];
        if ((layer->mPlaneAlpha == 1.0f) && (layer->mOverlayPriority >= ePriorityHigh)) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer has high or max priority (%d)", i, layer->mOverlayPriority);
            continue;
        }
        if (layer->mValidateCompositionType != HWC2_COMPOSITION_CLIENT) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer changed", i);
            if (layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS)
                exynosCompositionChanged = true;
            else {
                if (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)
                    mWindowNumUsed--;
            }
            layer->resetAssignedResource();
            layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            layer->mOverlayInfo |= eSandwitchedBetweenGLES;
        }
    }

    /* Check Exynos Composition info is changed */
    if (exynosCompositionChanged) {
        DISPLAY_LOGD(eDebugResourceAssigning, "exynos composition [%d] - [%d] is changed",
                     mExynosCompositionInfo.mFirstIndex, mExynosCompositionInfo.mLastIndex);
        uint32_t newFirstIndex = ~0;
        int32_t newLastIndex = -1;

        if ((mExynosCompositionInfo.mFirstIndex < 0) || (mExynosCompositionInfo.mLastIndex < 0)) {
            HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: mExynosCompositionInfo.mHasCompositionLayer should be true(%d) "
                                                     "but index is not valid (firstIndex: %d, lastIndex: %d)",
                     __func__, mExynosCompositionInfo.mHasCompositionLayer,
                     mExynosCompositionInfo.mFirstIndex,
                     mExynosCompositionInfo.mLastIndex);
            return -EINVAL;
        }

        for (uint32_t i = 0; i < mLayers.size(); i++) {
            ExynosLayer *exynosLayer = mLayers[i];
            if (exynosLayer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS) {
                newFirstIndex = min(newFirstIndex, i);
                newLastIndex = max(newLastIndex, (int32_t)i);
            }
        }

        DISPLAY_LOGD(eDebugResourceAssigning, "changed exynos composition [%d] - [%d]",
                     newFirstIndex, newLastIndex);

        /* There is no exynos composition layer */
        if (newFirstIndex == (uint32_t)~0) {
            mExynosCompositionInfo.initializeInfos();
            ret = EXYNOS_ERROR_CHANGED;
        } else {
            mExynosCompositionInfo.mFirstIndex = newFirstIndex;
            mExynosCompositionInfo.mLastIndex = newLastIndex;
        }
        if (isExynosCompositionChanged != NULL)
            *isExynosCompositionChanged = 1;
    }

    DISPLAY_LOGD(eDebugResourceManager, "\tresult changeFlag(0x%8x)", ret);
    DISPLAY_LOGD(eDebugResourceManager, "\tClient composition(%d) range [%d] - [%d]",
                 mClientCompositionInfo.mHasCompositionLayer,
                 mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex);
    DISPLAY_LOGD(eDebugResourceManager, "\tExynos composition(%d) range [%d] - [%d]",
                 mExynosCompositionInfo.mHasCompositionLayer,
                 mExynosCompositionInfo.mFirstIndex, mExynosCompositionInfo.mLastIndex);

    return ret;
}
int32_t ExynosDisplay::removeClientCompositionLayer(uint32_t layerIndex) {
    int32_t ret = NO_ERROR;

    DISPLAY_LOGD(eDebugResourceManager, "[%d] - [%d] [%d] layer is removed from client composition",
                 mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex,
                 layerIndex);

    /* Only first layer or last layer can be removed */
    if ((mClientCompositionInfo.mHasCompositionLayer == false) ||
        ((mClientCompositionInfo.mFirstIndex != (int32_t)layerIndex) &&
         (mClientCompositionInfo.mLastIndex != (int32_t)layerIndex))) {
        DISPLAY_LOGE("removeClientCompositionLayer() error, [%d] - [%d], layer[%d]",
                     mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex,
                     layerIndex);
        return -EINVAL;
    }

    if (mClientCompositionInfo.mFirstIndex == mClientCompositionInfo.mLastIndex) {
        ExynosMPP *otfMPP = mClientCompositionInfo.mOtfMPP;
        if (otfMPP != NULL)
            otfMPP->resetAssignedState();
        else {
            DISPLAY_LOGE("mClientCompositionInfo.mOtfMPP is NULL");
            return -EINVAL;
        }
        mClientCompositionInfo.initializeInfos();
        mWindowNumUsed--;
    } else if ((int32_t)layerIndex == mClientCompositionInfo.mFirstIndex)
        mClientCompositionInfo.mFirstIndex++;
    else
        mClientCompositionInfo.mLastIndex--;

    DISPLAY_LOGD(eDebugResourceManager, "\tClient composition(%d) range [%d] - [%d]",
                 mClientCompositionInfo.mHasCompositionLayer,
                 mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex);

    return ret;
}

bool ExynosDisplay::hasClientComposition() {
    return mClientCompositionInfo.mHasCompositionLayer;
}

int32_t ExynosDisplay::handleSandwitchedExynosCompositionLayer(
    std::vector<int32_t> &highPriLayers, float totalUsedCapa,
    bool &invalidFlag, int32_t &changeFlag) {
    int32_t ret = NO_ERROR;
    ExynosMPP *m2mMPP = mExynosCompositionInfo.mM2mMPP;

    /* totalUsedCapa should be re-calculated
     * while sandwitched layeres are added to exynos composition.
     * It is because used capacity of blending MPP is updated
     * by added sandwitched layers.
     */
    totalUsedCapa -= m2mMPP->mUsedCapacity;
    invalidFlag = false;

    /* handle sandwiched layers */
    for (int32_t i = mExynosCompositionInfo.mFirstIndex; i <= mExynosCompositionInfo.mLastIndex; i++) {
        ExynosLayer *layer = mLayers[i];
        if (layer == NULL) {
            DISPLAY_LOGE("layer[%d] layer is null", i);
            continue;
        }

        if (layer->mOverlayPriority >= ePriorityHigh) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer has high priority", i);
            highPriLayers.push_back(i);
            continue;
        }

        if (layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS)
            continue;

        exynos_image src_img;
        exynos_image dst_img;
        layer->setSrcExynosImage(&src_img);
        layer->setDstExynosImage(&dst_img);
        layer->setExynosImage(src_img, dst_img);
        layer->setExynosMidImage(dst_img);
        bool isAssignable = false;
        if ((layer->mSupportedMPPFlag & m2mMPP->mLogicalType) != 0)
            isAssignable = m2mMPP->isAssignable(mDisplayInfo, src_img, dst_img,
                                                totalUsedCapa + m2mMPP->mUsedCapacity);

        if (layer->mValidateCompositionType == HWC2_COMPOSITION_CLIENT) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer is client composition", i);
            invalidFlag = true;
        } else if (((layer->mSupportedMPPFlag & m2mMPP->mLogicalType) == 0) ||
                   (isAssignable == false)) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer is not supported by G2D", i);
            invalidFlag = true;
            layer->resetAssignedResource();
            layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            if ((ret = addClientCompositionLayer(i)) < 0)
                return ret;
            changeFlag |= ret;
        } else if ((layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) ||
                   (layer->mValidateCompositionType == HWC2_COMPOSITION_INVALID)) {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer changed", i);
            layer->mOverlayInfo |= eSandwitchedBetweenEXYNOS;
            layer->resetAssignedResource();
            if ((ret = m2mMPP->assignMPP(mDisplayInfo, layer)) != NO_ERROR) {
                HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: %s MPP assignMPP() error (%d)",
                         __func__, m2mMPP->mName.c_str(), ret);
                return ret;
            }
            if (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)
                mWindowNumUsed--;
            layer->mValidateCompositionType = HWC2_COMPOSITION_EXYNOS;
            mExynosCompositionInfo.mFirstIndex = min(mExynosCompositionInfo.mFirstIndex, (int32_t)i);
            mExynosCompositionInfo.mLastIndex = max(mExynosCompositionInfo.mLastIndex, (int32_t)i);
        } else {
            DISPLAY_LOGD(eDebugResourceAssigning, "\t[%d] layer has known type (%d)", i, layer->mValidateCompositionType);
        }
    }
    return NO_ERROR;
}

int32_t ExynosDisplay::handleNestedClientCompositionLayer(int32_t &changeFlag) {
    /* Check if exynos comosition nests GLES composition */
    if (!(mClientCompositionInfo.mHasCompositionLayer) ||
        (mExynosCompositionInfo.mFirstIndex >= mClientCompositionInfo.mFirstIndex) ||
        (mClientCompositionInfo.mFirstIndex >= mExynosCompositionInfo.mLastIndex) ||
        (mExynosCompositionInfo.mFirstIndex >= mClientCompositionInfo.mLastIndex) ||
        (mClientCompositionInfo.mLastIndex >= mExynosCompositionInfo.mLastIndex))
        return NO_ERROR;

    int32_t ret = NO_ERROR;
    uint32_t isExynosCompositionChanged = 0;
    if ((mClientCompositionInfo.mFirstIndex - mExynosCompositionInfo.mFirstIndex) <
        (mExynosCompositionInfo.mLastIndex - mClientCompositionInfo.mLastIndex)) {
        mLayers[mExynosCompositionInfo.mFirstIndex]->resetAssignedResource();
        mLayers[mExynosCompositionInfo.mFirstIndex]->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
        if ((ret = addClientCompositionLayer(mExynosCompositionInfo.mFirstIndex,
                                             &isExynosCompositionChanged)) < 0)
            return ret;
        /* Update index only if index was not already changed by addClientCompositionLayer */
        if (isExynosCompositionChanged == 0)
            mExynosCompositionInfo.mFirstIndex = mClientCompositionInfo.mLastIndex + 1;
        changeFlag |= ret;
    } else {
        mLayers[mExynosCompositionInfo.mLastIndex]->resetAssignedResource();
        mLayers[mExynosCompositionInfo.mLastIndex]->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
        if ((ret = addClientCompositionLayer(mExynosCompositionInfo.mLastIndex,
                                             &isExynosCompositionChanged)) < 0)
            return ret;
        /* Update index only if index was not already changed by addClientCompositionLayer */
        if (isExynosCompositionChanged == 0)
            mExynosCompositionInfo.mLastIndex = (mClientCompositionInfo.mFirstIndex - 1);
        changeFlag |= ret;
    }
    return NO_ERROR;
}

int32_t ExynosDisplay::addExynosCompositionLayer(uint32_t layerIndex, float totalUsedCapa) {
    bool invalidFlag = false;
    int32_t changeFlag = NO_ERROR;
    int ret = 0;
    int32_t startIndex;
    int32_t endIndex;

    DISPLAY_LOGD(eDebugResourceManager, "[%d] layer is added to exynos composition", layerIndex);

    if (mExynosCompositionInfo.mHasCompositionLayer == false) {
        mExynosCompositionInfo.mFirstIndex = layerIndex;
        mExynosCompositionInfo.mLastIndex = layerIndex;
        mExynosCompositionInfo.mHasCompositionLayer = true;
        return EXYNOS_ERROR_CHANGED;
    } else {
        mExynosCompositionInfo.mFirstIndex = min(mExynosCompositionInfo.mFirstIndex, (int32_t)layerIndex);
        mExynosCompositionInfo.mLastIndex = max(mExynosCompositionInfo.mLastIndex, (int32_t)layerIndex);
    }

    DISPLAY_LOGD(eDebugResourceAssigning, "\tExynos composition range [%d] - [%d]",
                 mExynosCompositionInfo.mFirstIndex, mExynosCompositionInfo.mLastIndex);

    ExynosMPP *m2mMPP = mExynosCompositionInfo.mM2mMPP;

    if (m2mMPP == NULL) {
        DISPLAY_LOGE("exynosComposition m2mMPP is NULL");
        return -EINVAL;
    }

    auto checkIndexValidation = [&]() -> bool {
        return ((mExynosCompositionInfo.mFirstIndex >= 0) &&
                (mExynosCompositionInfo.mFirstIndex < (int)mLayers.size()) &&
                (mExynosCompositionInfo.mLastIndex >= 0) &&
                (mExynosCompositionInfo.mLastIndex < (int)mLayers.size()) &&
                (mExynosCompositionInfo.mFirstIndex <=
                 mExynosCompositionInfo.mLastIndex));
    };

    if (!checkIndexValidation()) {
        DISPLAY_LOGE("exynosComposition invalid index (%d), (%d)",
                     mExynosCompositionInfo.mFirstIndex,
                     mExynosCompositionInfo.mLastIndex);
        return -EINVAL;
    }

    std::vector<int32_t> highPriority;
    if ((ret = handleSandwitchedExynosCompositionLayer(highPriority,
                                                       totalUsedCapa, invalidFlag, changeFlag)) != NO_ERROR)
        return ret;

    if (invalidFlag) {
        DISPLAY_LOGD(eDebugResourceAssigning,
                     "\tClient composition range [%d] - [%d]",
                     mClientCompositionInfo.mFirstIndex,
                     mClientCompositionInfo.mLastIndex);
        DISPLAY_LOGD(eDebugResourceAssigning,
                     "\tExynos composition range [%d] - [%d], highPriorityNum[%zu]",
                     mExynosCompositionInfo.mFirstIndex,
                     mExynosCompositionInfo.mLastIndex, highPriority.size());
        if ((ret = handleNestedClientCompositionLayer(changeFlag)) != NO_ERROR)
            return ret;
    }

    if (m2mMPP->mLogicalType == MPP_LOGICAL_G2D_RGB) {
        for (uint32_t i = 0; i < highPriority.size(); i++) {
            if ((int32_t)highPriority[i] == mExynosCompositionInfo.mFirstIndex)
                mExynosCompositionInfo.mFirstIndex++;
            else if ((int32_t)highPriority[i] == mExynosCompositionInfo.mLastIndex)
                mExynosCompositionInfo.mLastIndex--;
        }
    }

    if (!checkIndexValidation()) {
        DISPLAY_LOGD(eDebugResourceAssigning, "\texynos composition is disabled,"
                                              "because of invalid index (%d, %d), size(%zu)",
                     mExynosCompositionInfo.mFirstIndex,
                     mExynosCompositionInfo.mLastIndex, mLayers.size());
        mExynosCompositionInfo.initializeInfos();
        changeFlag = EXYNOS_ERROR_CHANGED;
    }

    int32_t highPriorityCheck = 0;
    for (uint32_t i = 0; i < highPriority.size(); i++) {
        if ((mExynosCompositionInfo.mFirstIndex < (int32_t)highPriority[i]) &&
            ((int32_t)highPriority[i] < mExynosCompositionInfo.mLastIndex)) {
            highPriorityCheck = 1;
            break;
        }
    }

    if (highPriorityCheck && (m2mMPP->mLogicalType = MPP_LOGICAL_G2D_RGB)) {
        startIndex = mExynosCompositionInfo.mFirstIndex;
        endIndex = mExynosCompositionInfo.mLastIndex;
        DISPLAY_LOGD(eDebugResourceAssigning, "\texynos composition is disabled because of sandwitched max priority layer (%d, %d)",
                     mExynosCompositionInfo.mFirstIndex, mExynosCompositionInfo.mLastIndex);
        for (int32_t i = startIndex; i <= endIndex; i++) {
            if (mLayers[i]->mOverlayPriority >= ePriorityHigh)
                continue;

            mLayers[i]->resetAssignedResource();
            mLayers[i]->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            if ((ret = addClientCompositionLayer(i)) < 0)
                HWC_LOGE(mDisplayInfo.displayIdentifier, "%d layer: addClientCompositionLayer() fail", i);
        }
        mExynosCompositionInfo.initializeInfos();
        changeFlag = EXYNOS_ERROR_CHANGED;
    }

    DISPLAY_LOGD(eDebugResourceManager, "\tresult changeFlag(0x%8x)", changeFlag);
    DISPLAY_LOGD(eDebugResourceManager, "\tClient composition range [%d] - [%d]",
                 mClientCompositionInfo.mFirstIndex, mClientCompositionInfo.mLastIndex);
    DISPLAY_LOGD(eDebugResourceManager, "\tExynos composition range [%d] - [%d]",
                 mExynosCompositionInfo.mFirstIndex, mExynosCompositionInfo.mLastIndex);

    return changeFlag;
}

bool ExynosDisplay::windowUpdateExceptions() {
    if (mDpuData.enable_readback)
        return true;

    if (exynosHWCControl.windowUpdate != 1)
        return true;

    if (mGeometryChanged != 0) {
        DISPLAY_LOGD(eDebugWindowUpdate, "GEOMETRY chnaged 0x%" PRIx64 "",
                     mGeometryChanged);
        return true;
    }

    if ((mCursorIndex >= 0) && (mCursorIndex < (int32_t)mLayers.size())) {
        ExynosLayer *layer = mLayers[mCursorIndex];
        /* Cursor layer is enabled */
        if (layer->mExynosCompositionType == HWC2_COMPOSITION_DEVICE) {
            return true;
        }
    }

    if (mType == HWC_DISPLAY_VIRTUAL) {
        DISPLAY_LOGD(eDebugWindowUpdate, "virtual display can't support window update");
        return true;
    }

    if (mExynosCompositionInfo.mHasCompositionLayer) {
        DISPLAY_LOGD(eDebugWindowUpdate, "has exynos composition");
        return true;
    }
    if (mClientCompositionInfo.mHasCompositionLayer) {
        DISPLAY_LOGD(eDebugWindowUpdate, "has client composition");
        return true;
    }

    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mM2mMPP != NULL)
            return true;
        if (mLayers[i]->mLayerBuffer == NULL)
            return true;
        if (mLayers[i]->mTransform != 0)
            return true;
    }

    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        exynos_win_config_data &config = mDpuData.configs[i];
        exynos_win_config_data &lastConfig = mLastDpuData.configs[i];

        if ((config.plane_alpha != lastConfig.plane_alpha))
            return true;

        if (config.format.isYUV()) {
            if ((config.src.x % 2) || (config.src.y % 2) || (config.src.w % 2) || (config.src.h % 2) ||
                (config.dst.x % 2) || (config.dst.y % 2) || (config.dst.w % 2) || (config.dst.h % 2)) {
                return true;
            }
        }

        if (config.state == config.WIN_STATE_BUFFER) {
            if (config.src.w / config.dst.w != 1 || config.src.h / config.dst.h != 1) {
                DISPLAY_LOGD(eDebugWindowUpdate, "Skip reason : scaled");
                return true;
            }
        }
    }

    return false;
}

int ExynosDisplay::handleWindowUpdate() {
    mDpuData.enable_win_update = false;
    /* Init with full size */
    mDpuData.win_update_region.x = 0;
    mDpuData.win_update_region.w = mXres;
    mDpuData.win_update_region.y = 0;
    mDpuData.win_update_region.h = mYres;

    if (windowUpdateExceptions())
        return 0;

    hwc_rect mergedRect = {(int)mXres, (int)mYres, 0, 0};
    hwc_rect damageRect = {(int)mXres, (int)mYres, 0, 0};
    if (mergeDamageRect(mergedRect, damageRect) != NO_ERROR) {
        DISPLAY_LOGD(eDebugWindowUpdate, "Window update is canceled");
        return 0;
    }

    if (setWindowUpdate(mergedRect) != NO_ERROR) {
        DISPLAY_LOGD(eDebugWindowUpdate, "Window update is canceled");
        return 0;
    }

    return 0;
}

int ExynosDisplay::mergeDamageRect(hwc_rect &merge_rect, hwc_rect &damage_rect) {
    for (size_t i = 0; i < mLayers.size(); i++) {
        int32_t windowIndex = mLayers[i]->mWindowIndex;
        if ((windowIndex < 0) ||
            (windowIndex >= mDpuData.configs.size())) {
            DISPLAY_LOGE("%s:: layer[%zu] has invalid window index(%d)\n",
                         __func__, i, windowIndex);
            return -1;
        }

        int ret = 0;
        if ((ret = canApplyWindowUpdate(mLastDpuData, mDpuData, windowIndex)) < 0) {
            DISPLAY_LOGD(eDebugWindowUpdate, "Config data is changed, cannot apply window update");
            return -1;
        } else if (ret > 0) {
            damage_rect.left = mLayers[i]->mDisplayFrame.left;
            damage_rect.right = mLayers[i]->mDisplayFrame.right;
            damage_rect.top = mLayers[i]->mDisplayFrame.top;
            damage_rect.bottom = mLayers[i]->mDisplayFrame.bottom;
            DISPLAY_LOGD(eDebugWindowUpdate, "Skip layer (origin) : %d, %d, %d, %d",
                         damage_rect.left, damage_rect.top, damage_rect.right, damage_rect.bottom);
            merge_rect = expand(merge_rect, damage_rect);
            hwc_rect prevDst = {mLastDpuData.configs[windowIndex].dst.x, mLastDpuData.configs[windowIndex].dst.y,
                                mLastDpuData.configs[windowIndex].dst.x + (int)mLastDpuData.configs[windowIndex].dst.w,
                                mLastDpuData.configs[windowIndex].dst.y + (int)mLastDpuData.configs[windowIndex].dst.h};
            DISPLAY_LOGD(eDebugWindowUpdate, "prev rect(%d, %d, %d, %d)",
                         prevDst.left, prevDst.top, prevDst.right, prevDst.bottom);

            merge_rect = expand(merge_rect, prevDst);
            continue;
        }

        unsigned int excp = getLayerRegion(mLayers[i], damage_rect, eDamageRegionByDamage);
        if (excp == eDamageRegionPartial) {
            DISPLAY_LOGD(eDebugWindowUpdate, "layer(%zu) partial : %d, %d, %d, %d", i,
                         damage_rect.left, damage_rect.top, damage_rect.right, damage_rect.bottom);
            merge_rect = expand(merge_rect, damage_rect);
        } else if (excp == eDamageRegionSkip) {
            DISPLAY_LOGD(eDebugWindowUpdate, "layer(%zu) skip", i);
            continue;
        } else if (excp == eDamageRegionFull) {
            damage_rect.left = mLayers[i]->mDisplayFrame.left;
            damage_rect.top = mLayers[i]->mDisplayFrame.top;
            damage_rect.right = mLayers[i]->mDisplayFrame.right;
            damage_rect.bottom = mLayers[i]->mDisplayFrame.bottom;
            DISPLAY_LOGD(eDebugWindowUpdate, "Full layer update : %d, %d, %d, %d",
                         mLayers[i]->mDisplayFrame.left,
                         mLayers[i]->mDisplayFrame.top,
                         mLayers[i]->mDisplayFrame.right,
                         mLayers[i]->mDisplayFrame.bottom);
            merge_rect = expand(merge_rect, damage_rect);
        } else {
            DISPLAY_LOGD(eDebugWindowUpdate, "Window update is canceled, Skip reason (layer %zu) : %d", i, excp);
            return -1;
        }
    }

    return NO_ERROR;
}

unsigned int ExynosDisplay::getLayerRegion(ExynosLayer *layer, hwc_rect &rect_area, uint32_t regionType) {
    android::Vector<hwc_rect_t> hwcRects;
    size_t numRects = 0;

    rect_area.left = INT_MAX;
    rect_area.top = INT_MAX;
    rect_area.right = rect_area.bottom = 0;

    hwcRects = layer->mDamageRects;
    numRects = layer->mDamageNum;

    if ((numRects == 0) || (hwcRects.size() == 0))
        return eDamageRegionFull;

    if ((numRects == 1) && (hwcRects[0].left == 0) && (hwcRects[0].top == 0) &&
        (hwcRects[0].right == 0) && (hwcRects[0].bottom == 0))
        return eDamageRegionSkip;

    switch (regionType) {
    case eDamageRegionByDamage:
        for (size_t j = 0; j < hwcRects.size(); j++) {
            hwc_rect_t rect;

            if ((hwcRects[j].left < 0) || (hwcRects[j].top < 0) ||
                (hwcRects[j].right < 0) || (hwcRects[j].bottom < 0) ||
                (hwcRects[j].left >= hwcRects[j].right) || (hwcRects[j].top >= hwcRects[j].bottom) ||
                (hwcRects[j].right - hwcRects[j].left > WIDTH(layer->mSourceCrop)) ||
                (hwcRects[j].bottom - hwcRects[j].top > HEIGHT(layer->mSourceCrop))) {
                rect_area.left = INT_MAX;
                rect_area.top = INT_MAX;
                rect_area.right = rect_area.bottom = 0;
                return eDamageRegionFull;
            }

            rect.left = layer->mDisplayFrame.left + hwcRects[j].left - layer->mSourceCrop.left;
            rect.top = layer->mDisplayFrame.top + hwcRects[j].top - layer->mSourceCrop.top;
            rect.right = layer->mDisplayFrame.left + hwcRects[j].right - layer->mSourceCrop.left;
            rect.bottom = layer->mDisplayFrame.top + hwcRects[j].bottom - layer->mSourceCrop.top;
            DISPLAY_LOGD(eDebugWindowUpdate, "Display frame : %d, %d, %d, %d", layer->mDisplayFrame.left,
                         layer->mDisplayFrame.top, layer->mDisplayFrame.right, layer->mDisplayFrame.bottom);
            DISPLAY_LOGD(eDebugWindowUpdate, "hwcRects : %d, %d, %d, %d", hwcRects[j].left,
                         hwcRects[j].top, hwcRects[j].right, hwcRects[j].bottom);
            adjustRect(rect, INT_MAX, INT_MAX);
            /* Get sums of rects */
            rect_area = expand(rect_area, rect);
        }
        return eDamageRegionPartial;
        break;
    case eDamageRegionByLayer:
        if (layer->mLastLayerBuffer != layer->mLayerBuffer)
            return eDamageRegionFull;
        else
            return eDamageRegionSkip;
        break;
    default:
        HWC_LOGE(mDisplayInfo.displayIdentifier, "%s:: Invalid regionType (%d)", __func__, regionType);
        return eDamageRegionError;
        break;
    }

    return eDamageRegionFull;
}

uint32_t ExynosDisplay::getRestrictionIndex(const ExynosFormat &format) {
    if (format.isRgb())
        return RESTRICTION_RGB;
    else
        return RESTRICTION_YUV;
}

void ExynosDisplay::closeFencesForSkipFrame(rendering_state renderingState) {
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mAcquireFence != -1) {
            mLayers[i]->mAcquireFence = mFenceTracer.fence_close(mLayers[i]->mAcquireFence,
                                                                 mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER,
                                                                 "display::closeFencesForSkipFrame: layer acq_fence");
        }
    }

    if (mDpuData.readback_info.rel_fence >= 0) {
        mDpuData.readback_info.rel_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.rel_fence,
                                     mDisplayInfo.displayIdentifier, FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                     "display::closeFencesForSkipFrame: readback rel_fence");
    }
    if (mDpuData.readback_info.acq_fence >= 0) {
        mDpuData.readback_info.acq_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.acq_fence,
                                     mDisplayInfo.displayIdentifier, FENCE_TYPE_READBACK_ACQUIRE, FENCE_IP_DPP,
                                     "display::closeFencesForSkipFrame: readback acq_fence");
    }

    if (renderingState >= RENDERING_STATE_VALIDATED) {
        if (mDisplayControl.earlyStartMPP == true) {
            if (mExynosCompositionInfo.mHasCompositionLayer) {
                /*
                 * m2mMPP's release fence for dst buffer was set to
                 * mExynosCompositionInfo.mAcquireFence by startPostProcessing()
                 * in validate time.
                 * This fence should be passed to display driver
                 * but it wont't because this frame will not be presented.
                 * So fence should be closed.
                 */
                mExynosCompositionInfo.mAcquireFence = mFenceTracer.fence_close(mExynosCompositionInfo.mAcquireFence,
                                                                                mDisplayInfo.displayIdentifier, FENCE_TYPE_DST_ACQUIRE, FENCE_IP_G2D,
                                                                                "display::closeFencesForSkipFrame: exynos comp acq_fence");
            }

            for (size_t i = 0; i < mLayers.size(); i++) {
                exynos_image outImage;
                ExynosMPP *m2mMPP = mLayers[i]->mM2mMPP;
                if ((mLayers[i]->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) &&
                    (m2mMPP != NULL) &&
                    (m2mMPP->mAssignedDisplayInfo.displayIdentifier.id == mDisplayId) &&
                    (m2mMPP->getDstImageInfo(&outImage) == NO_ERROR)) {
                    if (m2mMPP->mPhysicalType == MPP_MSC) {
                        mFenceTracer.fence_close(outImage.acquireFenceFd, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_DST_ACQUIRE, FENCE_IP_MSC,
                                                 "display::closeFencesForSkipFrame: MSC outImage.acquireFenceFd");
                    } else if (m2mMPP->mPhysicalType == MPP_G2D) {
                        mFenceTracer.fence_close(outImage.acquireFenceFd, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_DST_ACQUIRE, FENCE_IP_G2D,
                                                 "display::closeFencesForSkipFrame: G2D outImage.acquireFenceFd");
                    } else {
                        DISPLAY_LOGE("[%zu] layer has invalid mppType(%d), acquireFenceFd(%d)",
                                     i, m2mMPP->mPhysicalType, outImage.acquireFenceFd);
                        mFenceTracer.fence_close(outImage.acquireFenceFd, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_DST_ACQUIRE, FENCE_IP_ALL);
                    }
                    m2mMPP->resetDstAcquireFence();
                    ALOGD("reset buf[%d], %d", m2mMPP->mCurrentDstBuf,
                          m2mMPP->mDstImgs[m2mMPP->mCurrentDstBuf].acrylicReleaseFenceFd);
                }
            }
        }
    }

    if (renderingState >= RENDERING_STATE_PRESENTED) {
        /* mAcquireFence is set after validate */
        mClientCompositionInfo.mAcquireFence = mFenceTracer.fence_close(mClientCompositionInfo.mAcquireFence,
                                                                        mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB,
                                                                        "display::closeFencesForSkipFrame: client comp acq_fence");
    }
}
void ExynosDisplay::closeFences() {
    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        if (mDpuData.configs[i].acq_fence != -1)
            mFenceTracer.fence_close(mDpuData.configs[i].acq_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_DPP,
                                     "display::closeFences: config.acq_fence");
        mDpuData.configs[i].acq_fence = -1;
        if (mDpuData.configs[i].rel_fence >= 0)
            mFenceTracer.fence_close(mDpuData.configs[i].rel_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_RELEASE, FENCE_IP_DPP,
                                     "display::closeFences: config.rel_fence");
        mDpuData.configs[i].rel_fence = -1;
    }
    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mReleaseFence > 0) {
            mFenceTracer.fence_close(mLayers[i]->mReleaseFence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_RELEASE, FENCE_IP_LAYER,
                                     "display::closeFences: layer rel_fence");
            mLayers[i]->mReleaseFence = -1;
        }
        if ((mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_DEVICE) &&
            (mLayers[i]->mM2mMPP != NULL)) {
            mLayers[i]->mM2mMPP->closeFences();
        }
    }
    if (mExynosCompositionInfo.mHasCompositionLayer) {
        if (mExynosCompositionInfo.mM2mMPP == NULL) {
            DISPLAY_LOGE("There is exynos composition, but m2mMPP is NULL");
            return;
        }
        mExynosCompositionInfo.mM2mMPP->closeFences();
    }

    for (size_t i = 0; i < mLayers.size(); i++) {
        if (mLayers[i]->mAcquireFence != -1) {
            mLayers[i]->mAcquireFence = mFenceTracer.fence_close(mLayers[i]->mAcquireFence,
                                                                 mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER,
                                                                 "display::closeFences: layer acq_fence");
        }
    }

    mExynosCompositionInfo.mAcquireFence = mFenceTracer.fence_close(mExynosCompositionInfo.mAcquireFence,
                                                                    mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_G2D,
                                                                    "display::closeFences: exynos comp acq_fence");
    mClientCompositionInfo.mAcquireFence = mFenceTracer.fence_close(mClientCompositionInfo.mAcquireFence,
                                                                    mDisplayInfo.displayIdentifier, FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_FB,
                                                                    "display::closeFences: client comp acq_fence");

    if (mDpuData.present_fence > 0)
        mFenceTracer.fence_close(mDpuData.present_fence, mDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                 "display::closeFences: dpu present_fence");
    mDpuData.present_fence = -1;

    mLastPresentFence = mFenceTracer.fence_close(mLastPresentFence, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                                 "display::closeFences: mLastPresentFence");

    if (mDpuData.readback_info.rel_fence >= 0) {
        mDpuData.readback_info.rel_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.rel_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                     "display::closeFences: readback rel_fence");
    }
    if (mDpuData.readback_info.acq_fence >= 0) {
        mDpuData.readback_info.acq_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.acq_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_READBACK_ACQUIRE, FENCE_IP_DPP,
                                     "display::closeFences: readback acq_fence");
    }
}

void ExynosDisplay::setHWCControl(uint32_t ctrl, int32_t val) {
    switch (ctrl) {
    case HWC_CTL_ENABLE_EXYNOSCOMPOSITION_OPT:
        mDisplayControl.enableExynosCompositionOptimization = (unsigned int)val;
        break;
    case HWC_CTL_USE_MAX_G2D_SRC:
        mDisplayControl.useMaxG2DSrc = (unsigned int)val;
        break;
    case HWC_CTL_ENABLE_EARLY_START_MPP:
        mDisplayControl.earlyStartMPP = (unsigned int)val;
        break;
    default:
        DISPLAY_LOGE("%s: unsupported HWC_CTL (%d)", __func__, ctrl);
        break;
    }
}

int32_t ExynosDisplay::getHdrCapabilities(uint32_t *outNumTypes,
                                          int32_t *outTypes, float *outMaxLuminance,
                                          float *outMaxAverageLuminance, float *outMinLuminance) {
    DISPLAY_LOGD(eDebugHWC, "HWC2: %s, %d", __func__, __LINE__);

    if (outNumTypes == NULL || outMaxLuminance == NULL ||
        outMaxAverageLuminance == NULL || outMinLuminance == NULL) {
        return HWC2_ERROR_BAD_PARAMETER;
    }

    if (outTypes == NULL) {
        /*
         * This function is always called twice.
         * outTypes is NULL in the first call and
         * outType is valid pointer in the second call.
         * Get information only in the first call.
         * Use saved information in the second call.
         */
        if (mDisplayInterface->updateHdrCapabilities(mHdrTypes, &mMaxLuminance,
                                                     &mMaxAverageLuminance, &mMinLuminance) != NO_ERROR)
            return HWC2_ERROR_BAD_CONFIG;
    }

    *outMaxLuminance = mMaxLuminance;
    *outMaxAverageLuminance = mMaxAverageLuminance;
    *outMinLuminance = mMinLuminance;

    if (mHdrCoefInterface) {
        mHdrTargetInfo.min_luminance = mMinLuminance;
        mHdrTargetInfo.max_luminance = mMaxLuminance;
        if (mColorMode == HAL_COLOR_MODE_NATIVE)
            mHdrTargetInfo.dataspace = HAL_DATASPACE_V0_SRGB;
        else
            mHdrTargetInfo.dataspace = colorModeToDataspace(mColorMode);
        mHdrCoefInterface->setTargetInfo(&mHdrTargetInfo);
    }

    if (mType == HWC_DISPLAY_EXTERNAL) {
        /* outType and outNumTypes of external display are updated on
           ExynosExternalDisplay::getHdrCapabilities. */
        return HWC2_ERROR_NONE;
    }

    uint32_t vectorSize = static_cast<uint32_t>(mHdrTypes.size());
    if (vectorSize == 0) {
        if (mHasHdr10AttrMPP) {
            mHdrTypes.push_back(HAL_HDR_HDR10);
            mHdrTypes.push_back(HAL_HDR_HLG);
        }
        if (mHasHdr10PlusAttrMPP)
            mHdrTypes.push_back(HAL_HDR_HDR10_PLUS);
        vectorSize = static_cast<uint32_t>(mHdrTypes.size());
    }

    if (outTypes != NULL) {
        if (*outNumTypes != vectorSize) {
            DISPLAY_LOGE("%s:: Invalid parameter (outNumTypes: %u, mHdrTypes.size: %u)",
                         __func__, *outNumTypes, vectorSize);
            return HWC2_ERROR_BAD_PARAMETER;
        }
        for (uint32_t i = 0; i < vectorSize; i++)
            outTypes[i] = mHdrTypes[i];

        ALOGI("%s: hdrTypeNum(%u), maxLuminance(%f), maxAverageLuminance(%f), minLuminance(%f)",
              __func__, vectorSize, *outMaxLuminance,
              *outMaxAverageLuminance, *outMinLuminance);
    } else {
        *outNumTypes = vectorSize;
    }

    return HWC2_ERROR_NONE;
}

void ExynosDisplay::increaseMPPDstBufIndex() {
    for (size_t i = 0; i < mLayers.size(); i++) {
        if ((mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_DEVICE) &&
            (mLayers[i]->mM2mMPP != NULL)) {
            mLayers[i]->mM2mMPP->increaseDstBuffIndex();
        }
    }

    if ((mExynosCompositionInfo.mHasCompositionLayer) &&
        (mExynosCompositionInfo.mM2mMPP != NULL)) {
        mExynosCompositionInfo.mM2mMPP->increaseDstBuffIndex();
    }
}

int32_t ExynosDisplay::getReadbackBufferAttributes(int32_t * /*android_pixel_format_t*/ outFormat,
                                                   int32_t * /*android_dataspace_t*/ outDataspace) {
    int32_t ret = mDisplayInterface->getReadbackBufferAttributes(outFormat, outDataspace);

    if (ret == HWC2_ERROR_NONE) {
        /* Interface didn't specific set dataspace */
        if (*outDataspace == HAL_DATASPACE_UNKNOWN)
            *outDataspace = colorModeToDataspace(mColorMode);
        /* Set default value */
        *outDataspace = getRefinedDataspace((int)*outFormat, (android_dataspace_t)*outDataspace);

        mDisplayControl.readbackSupport = true;
        ALOGI("readback info: format(0x%8x), dataspace(0x%8x)", *outFormat, *outDataspace);
    } else {
        mDisplayControl.readbackSupport = false;
        ALOGI("readback is not supported, ret(%d)", ret);
        ret = HWC2_ERROR_UNSUPPORTED;
    }
    return ret;
}

int32_t ExynosDisplay::setReadbackBuffer(buffer_handle_t buffer, int32_t releaseFence) {
    Mutex::Autolock lock(mDisplayMutex);
    int32_t ret = HWC2_ERROR_NONE;

    /* initialize readback buffer setting */
    if (mDpuData.enable_readback)
        mDpuData.enable_readback = false;
    if (mDpuData.readback_info.acq_fence >= 0) {
        mDpuData.readback_info.acq_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.acq_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_READBACK_ACQUIRE, FENCE_IP_DPP,
                                     "display::setReadbackBuffer: acq_fence");
        DISPLAY_LOGE("previous readback out fence is not delivered to framework");
    }

    if (buffer == nullptr)
        return HWC2_ERROR_BAD_PARAMETER;

    if (mDisplayControl.readbackSupport) {
        mDpuData.enable_readback = true;
    } else {
        DISPLAY_LOGE("readback is not supported but setReadbackBuffer is called, buffer(%p), releaseFence(%d)",
                     buffer, releaseFence);
        if (releaseFence >= 0)
            releaseFence = mFenceTracer.fence_close(releaseFence, mDisplayInfo.displayIdentifier,
                                                    FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                                    "display::setReadbackBuffer: releaseFence");
        mDpuData.enable_readback = false;
        ret = HWC2_ERROR_UNSUPPORTED;
    }

    setReadbackBufferInternal(buffer, releaseFence);

    return ret;
}

void ExynosDisplay::setReadbackBufferInternal(buffer_handle_t buffer, int32_t releaseFence) {
    if (mDpuData.readback_info.rel_fence >= 0) {
        mDpuData.readback_info.rel_fence =
            mFenceTracer.fence_close(mDpuData.readback_info.rel_fence, mDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                     "display::setReadbackBufferInternal: releaseFence");
        DISPLAY_LOGE("previous readback release fence is not delivered to display device");
    }

    if (releaseFence >= 0) {
        mFenceTracer.setFenceInfo(releaseFence, mDisplayInfo.displayIdentifier,
                                  FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB, FENCE_FROM);
    }
    mDpuData.readback_info.rel_fence = releaseFence;

    if (buffer != NULL)
        mDpuData.readback_info.handle = buffer;
}

int32_t ExynosDisplay::getReadbackBufferFence(int32_t *outFence) {
    /*
     * acq_fence was not set or
     * it was already closed by error or frame skip
     */
    if (mDpuData.readback_info.acq_fence < 0) {
        *outFence = -1;
        return HWC2_ERROR_UNSUPPORTED;
    }

    *outFence = mDpuData.readback_info.acq_fence;

    /* Fence will be closed by caller of this function */
    mFenceTracer.setFenceInfo(mDpuData.readback_info.acq_fence,
                              mDisplayInfo.displayIdentifier, FENCE_TYPE_READBACK_ACQUIRE,
                              FENCE_IP_DPP, FENCE_TO);
    mDpuData.readback_info.acq_fence = -1;
    return HWC2_ERROR_NONE;
}

void ExynosDisplay::initDisplayInterface(uint32_t __unused interfaceType,
                                         void *deviceData, size_t &deviceDataSize) {
    mDisplayInterface = std::make_unique<ExynosDisplayInterface>();
    mDisplayInterface->init(mDisplayInfo.displayIdentifier,
                            deviceData, deviceDataSize);
}

void ExynosDisplay::setDumpCount(uint32_t dumpCount) {
    if (mLayerDumpManager->isRunning()) {
        DISPLAY_LOGE("%s::Dump request is not compeleted", __func__);
        return;
    }
    mLayerDumpManager->run(dumpCount);
}

void ExynosDisplay::getDumpLayer() {
    int32_t frameIndex = mLayerDumpManager->getDumpFrameIndex();
    layerDumpFrameInfo *frameInfo = mLayerDumpManager->getLayerDumpFrameInfo(frameIndex);

    if ((frameInfo->layerDumpState == LAYER_DUMP_IDLE) ||
        (frameInfo->layerDumpState == LAYER_DUMP_DONE)) {
        return;
    }

    ATRACE_CALL();
    DISPLAY_LOGD(eDebugHWC, "debug_dump_source %s dumpCount=%d mDisplayId=%d", __func__, frameIndex, mDisplayId);

    frameInfo->layerDumpCnt = mLayers.size();

    for (size_t i = 0; i < mLayers.size(); i++) {
        buffer_handle_t hnd = mLayers[i]->mLayerBuffer;
        int32_t acqFence = mLayers[i]->mAcquireFence;

        ExynosGraphicBufferMeta gmeta(hnd);

        if (!hnd) {
            DISPLAY_LOGE("%s: [%d] handle is NULL", __func__, 0);
            continue;
        }

        unsigned long long start, end;
        start = systemTime(SYSTEM_TIME_MONOTONIC);
        DISPLAY_LOGD(eDebugHWC, "%s, memcpy.. Frame : %d, Layer : %zu,  %p, %d", __func__, frameIndex, i, mLayers[i]->mLayerBuffer, acqFence);

        if (mFenceTracer.fence_valid(acqFence)) {
            if (sync_wait(acqFence, 1000) < 0) {
                DISPLAY_LOGE("debug_dump_source %s:: [%d] sync wait failed", __func__, 0);
                return;
            }
        }

        layerDumpLayerInfo layerInfo;

        if (getBufLength(hnd, 4, layerInfo.bufferLength, gmeta.format, gmeta.stride, gmeta.vstride) != NO_ERROR) {
            DISPLAY_LOGE("debug_dump_source %s:: invalid bufferLength(%zu, %zu, %zu, %zu), format(0x%8x)", __func__,
                         layerInfo.bufferLength[0], layerInfo.bufferLength[1], layerInfo.bufferLength[2], layerInfo.bufferLength[3], gmeta.format);
            return;
        }

        layerInfo.bufferNum = getBufferNumOfFormat(gmeta.format);
        layerInfo.layerType = getTypeOfFormat(gmeta.format);
        layerInfo.compressionType = (mLayers[i]->mCompressionInfo.type == COMP_TYPE_NONE) ? true : false;
        layerInfo.format = gmeta.format;
        layerInfo.stride = gmeta.stride;
        layerInfo.vStride = gmeta.vstride;
        layerInfo.width = gmeta.width;
        layerInfo.height = gmeta.height;
        layerInfo.compositionType = mLayers[i]->mCompositionType;

        int bufFds[4];
        bufFds[0] = gmeta.fd;
        bufFds[1] = gmeta.fd1;
        bufFds[2] = gmeta.fd2;

        for (int pNo = 0; pNo < layerInfo.bufferNum; pNo++) {
            if ((bufFds[pNo] != -1) && layerInfo.bufferLength[pNo] > 0) {
                void *_buf = mmap(0, layerInfo.bufferLength[0], PROT_READ | PROT_WRITE, MAP_SHARED, bufFds[0], 0);
                if (_buf != MAP_FAILED && _buf != NULL) {
                    layerInfo.planeRawData[0] = malloc(layerInfo.bufferLength[0]);
                    if (layerInfo.planeRawData[0] != NULL)
                        memcpy(layerInfo.planeRawData[0], _buf, layerInfo.bufferLength[0]);
                    munmap(_buf, layerInfo.bufferLength[0]);
                }
            }
        }

        frameInfo->layerInfo[i] = layerInfo;

        end = systemTime(SYSTEM_TIME_MONOTONIC);
        DISPLAY_LOGD(eDebugHWC, "%s, memcpy.. Done Frame : %d, Layer : %zu, %p, %d, time %lld",
                     __func__, frameIndex, i, mLayers[i]->mLayerBuffer,
                     acqFence, (unsigned long long)end - start);
    }

    frameInfo->layerDumpState = LAYER_DUMP_DONE;
}

void ExynosDisplay::dumpLayers() {
    int32_t frameIndex = mLayerDumpManager->getDumpFrameIndex();
    int32_t maxIndex = mLayerDumpManager->getDumpMaxIndex();
    DISPLAY_LOGD(eDebugHWC, "debug_dump_source finished dump file index=%d", frameIndex);
    for (int j = 0; j <= maxIndex; j++) {
        layerDumpFrameInfo *temp = mLayerDumpManager->getLayerDumpFrameInfo(j);
        for (int i = 0; i < temp->layerDumpCnt; i++) {
            DISPLAY_LOGD(eDebugHWC, "%s, writing.. frame : %d, layer : %d", __func__, j, i);
            writeDumpData(j, i, temp, &temp->layerInfo[i]);
        }
    }

    for (int j = 0; j <= maxIndex; j++) {
        layerDumpFrameInfo *temp = mLayerDumpManager->getLayerDumpFrameInfo(j);
        for (int i = 0; i < temp->layerDumpCnt; i++) {
            for (int k = 0; k < 3; k++) {
                layerDumpLayerInfo *layerInfo = &temp->layerInfo[i];
                if (layerInfo->planeRawData[k] != nullptr) {
                    free(layerInfo->planeRawData[k]);
                    layerInfo->planeRawData[k] = nullptr;
                }
            }
        }
    }
    mLayerDumpManager->stop();
}

void ExynosDisplay::writeDumpData(int32_t frameNo, int32_t layerNo,
                                  layerDumpFrameInfo *frameInfo, layerDumpLayerInfo *layerInfo) {
    FILE *fp = NULL;
    size_t result = 0;
    char filePath[MAX_DEV_NAME];
    uint32_t bufferNum = layerInfo->bufferNum;
    int32_t format = layerInfo->format;
    uint32_t compressionType = layerInfo->compressionType;
    int32_t compositionType = layerInfo->compositionType;

    if (isFormatRgb(format)) {
        DISPLAY_LOGD(eDebugHWC, "debug_dump_source rgb data enter");
        if (compressionType == COMP_TYPE_AFBC)
            sprintf(filePath, "%s/afbc_displayid_%u_frame_%03d_layer_%02d_format_%d_compressed_%d_comtype_%d_%dx%d.raw", ERROR_LOG_PATH0, mDisplayId, frameNo,
                    layerNo, format, 1, compositionType, layerInfo->stride, layerInfo->vStride);
        else
            sprintf(filePath, "%s/displayid_%u_frame_%03d_layer_%02d_format_%d_compressed_%d_comtype_%d_%dx%d.raw", ERROR_LOG_PATH0, mDisplayId, frameNo,
                    layerNo, format, 0, compositionType, layerInfo->stride, layerInfo->vStride);

        fp = fopen(filePath, "w+");
        if (fp) {
            result = fwrite(layerInfo->planeRawData[0], layerInfo->bufferLength[0], 1, fp);
            fclose(fp);
        }

        /* make afbc decoding script file */
        if (compressionType == COMP_TYPE_AFBC) {
            FILE *scfp = NULL;
            String8 scriptText;
            char scriptFilePath[MAX_DEV_NAME];

            sprintf(scriptFilePath, "%s/afbcdec.sh", ERROR_LOG_PATH0);
            scfp = fopen(scriptFilePath, "a+");
            if (scfp != NULL) {
                scriptText.appendFormat("./afbcdec -i afbc_displayid_%u_frame_%03d_layer_%02d_format_%d_compressed_%d_comtype_%d_%dx%d.raw ",
                                        mDisplayId, frameNo, layerNo, format, 1, compositionType, layerInfo->stride, layerInfo->vStride);

                scriptText.appendFormat("-o displayid_%u_frame_%03d_layer_%02d_format_%d_compressed_%d_comtype_%d_%dx%d.raw ",
                                        mDisplayId, frameNo, layerNo, format, 0, compositionType, layerInfo->stride, layerInfo->vStride);

                if (format == HAL_PIXEL_FORMAT_RGB_565)
                    scriptText.appendFormat("-h %dx%d_r5g6b5_0_0_0_0\n", layerInfo->stride, layerInfo->vStride);
                else if (format == HAL_PIXEL_FORMAT_RGBA_1010102)
                    scriptText.appendFormat("-h %dx%d_r10g10b10a2_0_0_0_0\n", layerInfo->stride, layerInfo->vStride);
                else
                    scriptText.appendFormat("-h %dx%d_r8g8b8a8_0_0_0_0\n", layerInfo->stride, layerInfo->vStride);

                fwrite(scriptText.c_str(), 1, scriptText.size(), scfp);
                fclose(scfp);
            }
        }

        DISPLAY_LOGD(eDebugHWC, "debug_dump_source Frame Dump %s: is %s result=%s mLayers.size=%zu", filePath, result ? "Successful" : "Failed", result ? "1" : strerror(errno), mLayers.size());
    }

    if (isFormatYUV(format)) {
        sprintf(filePath, "%s/displayid_%u_frame_%03d_layer_%02d_format_%d_compressed_%d_comtype_%d_%dx%d.raw", ERROR_LOG_PATH0, mDisplayId, frameNo,
                layerNo, format, 0, compositionType, layerInfo->stride, layerInfo->vStride);

        //forbit apend buffer when file has exited.so before each dumping,clear file
        fp = fopen(filePath, "w+");
        if (fp) {
            fclose(fp);
        }
        for (uint32_t start = 0; start < bufferNum; start++) {
            DISPLAY_LOGD(eDebugHWC, "debug_dump_source yuv data enter start_times=%d", start);
            fp = fopen(filePath, "a+");

            if (fp) {
                int align_width = layerInfo->stride;
                int width = layerInfo->width;
                int height = start == 0 ? layerInfo->height : layerInfo->height / 2;
                if (bufferNum == 3 && start != 0) {
                    height = layerInfo->height / 4;
                }
                result = yuvWriteByLines(layerInfo->planeRawData[0], align_width, width, height, fp);
                fclose(fp);
            }
            DISPLAY_LOGD(eDebugHWC, "debug_dump_source Frame Dump %s: is %s result=%s mLayers.size=%zu", filePath, result ? "Successful" : "Failed", result ? "1" : strerror(errno), mLayers.size());
        }
    }
}

size_t ExynosDisplay::yuvWriteByLines(void *temp, int align_Width, int original_Width, int original_Height, FILE *fp) {
    size_t result = 0;

    DISPLAY_LOGD(eDebugHWC, "debug_dump_source yuvWriteByLines enter align_Width=%d original_Width=%d, original_Height=%d",
                 align_Width, original_Width, original_Height);

    for (int h = 0; h < original_Height; h++) {
        result = fwrite((char *)temp + h * align_Width, original_Width, 1, fp);
    }
    return result;
}

void ExynosDisplay::setPresentState() {
    mRenderingState = RENDERING_STATE_PRESENTED;
    mRenderingStateFlags.presentFlag = true;
}

void ExynosDisplay::requestHiberExit() {
    if ((mHiberState.hiberExitFd == NULL) ||
        (mHiberState.exitRequested))
        return;

    uint32_t val;
    size_t size;
    rewind(mHiberState.hiberExitFd);
    size = fread(&val, 4, 1, mHiberState.hiberExitFd);
    HDEBUGLOGD(eDebugWinConfig, "hiber exit read size = %lu", (unsigned long)size);
    mHiberState.exitRequested = true;

    return;
}

void ExynosDisplay::initHiberState() {
    if (mHiberState.hiberExitFd == NULL)
        return;
    mHiberState.exitRequested = false;
    return;
}

int32_t ExynosDisplay::updateColorConversionInfo() {
    int32_t ret = NO_ERROR;
    if (mHdrCoefInterface == NULL) {
        return NO_ERROR;
    }

    bool hasHdrLayer =
        mDisplayInfo.hdrLayersIndex.size() ? true : false;
    mHdrCoefInterface->initHdrCoefBuildup(HDR_HW_DPU);
    mHdrCoefInterface->setHDRlayer(hasHdrLayer);

    auto setHdrCoefLayerInfo = [=, this](ExynosMPP *otfMPP, exynos_image &image,
                                   enum RenderSource renderSource) -> int32_t {
        /* If getHDRException is true, setLayerInfo is bypassed. */
        HdrLayerInfo hdrLayerInfo;
        getHdrLayerInfo(image, renderSource, &hdrLayerInfo);
#ifdef HDR_IF_VER
        return mHdrCoefInterface->setLayerInfo(otfMPP->mChId, &hdrLayerInfo);
#else
        return mHdrCoefInterface->setLayerInfo(otfMPP->mChId, hdrLayerInfo.dataspace,
                                               hdrLayerInfo.static_metadata, sizeof(hdrLayerInfo.static_metadata),
                                               hdrLayerInfo.dynamic_metadata, sizeof(hdrLayerInfo.dynamic_metadata),
                                               (image.blending == HWC2_BLEND_MODE_PREMULTIPLIED),
                                               hdrLayerInfo.bpc, renderSource, hdrLayerInfo.tf_matrix, getHDRException());
#endif
    };

    int32_t tmpRet = NO_ERROR;
    ExynosMPP *otfMPP = mExynosCompositionInfo.mOtfMPP;
    if (otfMPP != nullptr) {
        if ((tmpRet = setHdrCoefLayerInfo(otfMPP, mExynosCompositionInfo.mSrcImg,
                                          REND_G2D)) != NO_ERROR) {
            DISPLAY_LOGE("%s:: ExynosComposition setHdrCoefLayerInfo() "
                         "error(%d)",
                         __func__, tmpRet);
            ret = tmpRet;
        }
    }
    otfMPP = mClientCompositionInfo.mOtfMPP;
    if (otfMPP != nullptr) {
        if ((tmpRet = setHdrCoefLayerInfo(otfMPP, mClientCompositionInfo.mSrcImg,
                                          REND_GPU)) != NO_ERROR) {
            DISPLAY_LOGE("%s:: ClientComposition setHdrCoefLayerInfo() "
                         "error(%d)",
                         __func__, tmpRet);
            ret = tmpRet;
        }
    }
    for (auto layer : mLayers) {
        otfMPP = layer->mOtfMPP;
        if (otfMPP == nullptr)
            continue;
        if ((tmpRet = setHdrCoefLayerInfo(otfMPP, layer->mSrcImg,
                                          REND_ORI)) != NO_ERROR) {
            DISPLAY_LOGE("%s:: layer setHdrCoefLayerInfo() error(%d)",
                         __func__, tmpRet);
            ret = tmpRet;
        }
    }

    return ret;
}

void ExynosDisplay::checkLayersForRevertingDR(uint64_t &geometryChanged) {
    Mutex::Autolock lock(mDRMutex);

    if (mDynamicRecompMode != DEVICE_TO_CLIENT)
        return;

    for (uint32_t i = 0; i < mLayers.size(); i++) {
        if ((mLayers[i]->mLastLayerBuffer != mLayers[i]->mLayerBuffer) || geometryChanged) {
            mDynamicRecompMode = CLIENT_TO_DEVICE;
            DISPLAY_LOGD(eDebugDynamicRecomp, "[DYNAMIC_RECOMP] CLIENT TO DEVICE");
            setGeometryChanged(GEOMETRY_DISPLAY_DYNAMIC_RECOMPOSITION, geometryChanged);
            return;
        }
    }
    setGeometryChanged(GEOMETRY_DISPLAY_DYNAMIC_RECOMPOSITION, geometryChanged);
}

#ifdef USE_DQE_INTERFACE
bool ExynosDisplay::needDqeSetting() {
    /* If dqe interface is changed to pass HDR info,
     * we need to consider change loop procedure,
     * like top to bottom or bottom to top.
     */
    for (auto i : mDisplayInfo.hdrLayersIndex) {
        if (mLayers[i]->mExynosCompositionType == HWC2_COMPOSITION_DEVICE)
            return true;
    }
    return false;
}

void ExynosDisplay::setDqeCoef(int fd) {
    mDpuData.fd_dqe = fd;
}
#endif

constexpr int maxWaitTime = 20000; /* 20s */
void ExynosDisplay::waitPreviousFrameDone(int fence) {
    struct timeval tv_s, tv_e;
    long timediff;

    /* wait for 5 vsync */
    int32_t waitTime = mVsyncPeriod / 1000000 * 5;
    gettimeofday(&tv_s, NULL);
    if (mFenceTracer.fence_valid(fence)) {
        ATRACE_CALL();
        if (sync_wait(fence, waitTime) < 0) {
            DISPLAY_LOGW("%s:: fence(%d) is not released during (%d ms)",
                         __func__, fence, waitTime);
            if (sync_wait(fence, maxWaitTime) < 0) {
                DISPLAY_LOGE("%s:: fence sync wait error (%d)", __func__, fence);
            } else {
                gettimeofday(&tv_e, NULL);
                tv_e.tv_usec += (tv_e.tv_sec - tv_s.tv_sec) * 1000000;
                timediff = tv_e.tv_usec - tv_s.tv_usec;
                DISPLAY_LOGE("%s:: winconfig is delayed over 5 vysnc (fence:%d)(time:%ld)",
                             __func__, fence, timediff);
            }
        }
    }
}

int32_t ExynosDisplay::getDisplayInfo(DisplayInfo &dispInfo) {
    dispInfo.displayIdentifier.id = mDisplayId;
    dispInfo.displayIdentifier.type = mType;
    dispInfo.displayIdentifier.index = mIndex;
    dispInfo.displayIdentifier.name = mDisplayName;
    dispInfo.displayIdentifier.deconNodeName = mDeconNodeName;
    dispInfo.xres = mXres;
    dispInfo.yres = mYres;
    dispInfo.colorMode = mColorMode;
    dispInfo.hdrLayersIndex.clear();
    dispInfo.drmLayersIndex.clear();
    for (uint32_t i = 0; i < mLayers.size(); i++) {
        if (hasHdrInfo(mLayers[i]->mDataSpace))
            dispInfo.hdrLayersIndex.push_back(i);
        if (getDrmMode(mLayers[i]->mLayerBuffer) != NO_DRM)
            dispInfo.drmLayersIndex.push_back(i);
    }
    dispInfo.workingVsyncPeriod = mWorkingVsyncInfo.vsyncPeriod ? mWorkingVsyncInfo.vsyncPeriod : mVsyncPeriod;
    dispInfo.useDpu = mUseDpu;
    dispInfo.minLuminance = mMinLuminance;
    dispInfo.maxLuminance = mMaxLuminance;
    dispInfo.adjustDisplayFrame = mDisplayControl.adjustDisplayFrame;
    dispInfo.cursorSupport = mDisplayControl.cursorSupport;
    dispInfo.skipM2mProcessing = mDisplayControl.skipM2mProcessing;
    dispInfo.baseWindowIndex = mBaseWindowIndex;
    dispInfo.defaultDMA = mDefaultDMA;

    return NO_ERROR;
}

int32_t ExynosDisplay::setColorConversionInfo(ExynosMPP *otfMPP) {
    if (otfMPP == nullptr)
        return -EINVAL;
    if (mHdrCoefInterface == nullptr)
        return NO_ERROR;

    struct hdrCoefParcel data;
    data.hdrCoef = otfMPP->mHdrCoefAddr;
    return mHdrCoefInterface->getHdrCoefData(HDR_HW_DPU, otfMPP->mChId, &data);
}

void ExynosDisplay::setSrcAcquireFences() {
    for (auto layer : mLayers) {
        // Layer's acquire fence from SF
        layer->setSrcAcquireFence();
    }
}

void ExynosDisplay::handleVsync(uint64_t timestamp) {
    Mutex::Autolock lock(mDisplayMutex);
    if (timestamp == 0)
        getDisplayVsyncTimestamp(&timestamp);

    bool configApplied = true;

    if (mConfigRequestState == hwc_request_state_t::SET_CONFIG_STATE_REQUESTED) {
        hwc2_vsync_period_t vsyncPeriod;
        if (mDisplayInterface->getDisplayVsyncPeriod(&vsyncPeriod) ==
            HWC2_ERROR_NONE) {
            /*
             * mDesiredVsyncPeriod is nanoseconds
             * Compare with milliseconds
             */
            if (vsyncPeriod / microsecsPerSec ==
                mVsyncCallback.getDesiredVsyncPeriod() / microsecsPerSec)
                configApplied = true;
            else
                configApplied = false;
        } else {
            configApplied = mVsyncCallback.Callback(timestamp);
        }
        if (configApplied) {
            if (mVsyncCallback.getDesiredVsyncPeriod()) {
                resetConfigRequestState();
                mVsyncCallback.resetDesiredVsyncPeriod();
            }
            /*
             * Disable vsync if vsync config change is done
             */
            if (!mVsyncCallback.getVSyncEnabled()) {
                setVsyncEnabledInternal(false);
                mVsyncCallback.resetVsyncTimeStamp();
            }
        } else {
            updateConfigRequestAppliedTime();
        }
    }

    if (mDisplayId == getDisplayId(HWC_DISPLAY_PRIMARY, 0)) {
        hwc2_vsync_period_t curPeriod;
        getDisplayVsyncPeriodInternal(&curPeriod);
        ATRACE_INT("fps", std::chrono::nanoseconds(1s).count() / curPeriod);
    }

    if (!mVsyncCallback.getVSyncEnabled()) {
        return;
    }

    if (mIsVsyncDisplay) {
        /* Vsync callback must pass display ID of primary display 0. */
        auto vsyncCallbackInfo =
            mCallbackInfos[HWC2_CALLBACK_VSYNC];
        if (vsyncCallbackInfo.funcPointer &&
            vsyncCallbackInfo.callbackData)
            ((HWC2_PFN_VSYNC)vsyncCallbackInfo.funcPointer)(
                vsyncCallbackInfo.callbackData,
                getDisplayId(HWC_DISPLAY_PRIMARY, 0), timestamp);

        auto vsync_2_4CallbackInfo =
            mCallbackInfos[HWC2_CALLBACK_VSYNC_2_4];
        if (vsync_2_4CallbackInfo.funcPointer &&
            vsync_2_4CallbackInfo.callbackData)
            ((HWC2_PFN_VSYNC_2_4)vsync_2_4CallbackInfo.funcPointer)(
                vsync_2_4CallbackInfo.callbackData,
                getDisplayId(HWC_DISPLAY_PRIMARY, 0), timestamp, mVsyncPeriod);
    }
}

void ExynosDisplay::invalidate() {
    HWC2_PFN_REFRESH callbackFunc =
        (HWC2_PFN_REFRESH)mCallbackInfos[HWC2_CALLBACK_REFRESH].funcPointer;
    if (callbackFunc != NULL)
        callbackFunc(mCallbackInfos[HWC2_CALLBACK_REFRESH].callbackData,
                     getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    else
        DISPLAY_LOGE("%s:: refresh callback is not registered", __func__);
}

void ExynosDisplay::hotplug() {
    DISPLAY_LOGD(eDebugHWC, "call hotplug callback");

    HWC2_PFN_HOTPLUG callbackFunc =
        (HWC2_PFN_HOTPLUG)mCallbackInfos[HWC2_CALLBACK_HOTPLUG].funcPointer;
    if (callbackFunc != NULL)
        callbackFunc(mCallbackInfos[HWC2_CALLBACK_HOTPLUG].callbackData, mDisplayId,
                     mHpdStatus ? HWC2_CONNECTION_CONNECTED : HWC2_CONNECTION_DISCONNECTED);
    else
        DISPLAY_LOGE("%s:: hotplug callback is not registered", __func__);

    ALOGI("HPD callback(%s, mDisplayId %d) was called", mHpdStatus ? "connection" : "disconnection", mDisplayId);
}

bool ExynosDisplay::checkHotplugEventUpdated(bool &hpdStatus) {
    if ((mType != HWC_DISPLAY_EXTERNAL) &&
        (mDisplayInterface->mType == INTERFACE_TYPE_FB)) {
        return false;
    }

    hpdStatus = mDisplayInterface->readHotplugStatus();

    DISPLAY_LOGI("[%s] mDisplayId(%d), mIndex(%d), HPD Status(previous :%d, current : %d)", __func__, mDisplayId, mIndex, mHpdStatus, hpdStatus);

    if (mHpdStatus == hpdStatus)
        return false;
    else
        return true;
}

void ExynosDisplay::handleHotplugEvent(bool hpdStatus) {
    {
        Mutex::Autolock lock(mDisplayMutex);
        mHpdStatus = hpdStatus;
        if (!mHpdStatus)
            mDisplayInterface->onDisplayRemoved();
    }
}

bool ExynosDisplay::isSupportLibHdrApi() {
    if (mHdrCoefInterface)
        return true;
    else
        return false;
}

bool ExynosDisplay::needHdrProcessing(exynos_image &srcImg, exynos_image &dstImg) {
    if (!mHdrCoefInterface)
        return false;
    HdrLayerInfo hdrLayerInfo;
    getHdrLayerInfo(srcImg, REND_ORI, &hdrLayerInfo);
#ifdef HDR_IF_VER
    return mHdrCoefInterface->needHdrProcessing(&hdrLayerInfo);
#else
    return false;
#endif
}

uint32_t ExynosDisplay::getHdrLayerInfo(exynos_image img, enum RenderSource renderSource, HdrLayerInfo *outHdrLayerInfo) {
    ExynosHdrStaticInfo *staticMetadata = NULL;
    ExynosHdrDynamicInfo *dynamicMetadata = NULL;
    if (hasHdr10Plus(img) && img.metaParcel)
        dynamicMetadata = &(img.metaParcel->sHdrDynamicInfo);
    if (hasHdrInfo(img) && img.metaParcel)
        staticMetadata = &(img.metaParcel->sHdrStaticInfo);

    enum HdrBpc bppFormat = HDR_BPC_8;
    if (img.exynosFormat.is10Bit())
        bppFormat = HDR_BPC_10;

    float *layerColorTransformMatrix = nullptr;
    // If layer color transform isn't needed, send null ptr to hdr coef interface.
    if (img.needColorTransform)
        layerColorTransformMatrix = (float *)&img.colorTransformMatrix;

    outHdrLayerInfo->dataspace = getRefinedDataspace(img.exynosFormat.halFormat(), img.dataSpace);
    outHdrLayerInfo->static_metadata = staticMetadata;
    outHdrLayerInfo->static_len = sizeof(ExynosHdrStaticInfo);
    outHdrLayerInfo->dynamic_metadata = dynamicMetadata;
    outHdrLayerInfo->dynamic_len = sizeof(ExynosHdrDynamicInfo);
    outHdrLayerInfo->premult_alpha = img.blending == HWC2_BLEND_MODE_PREMULTIPLIED;
    outHdrLayerInfo->bpc = bppFormat;
    outHdrLayerInfo->source = renderSource;
    outHdrLayerInfo->tf_matrix = layerColorTransformMatrix;
    outHdrLayerInfo->bypass = getHDRException();

    return NO_ERROR;
}

void ExynosDisplay::resetForDestroyClient() {
    if (!mPlugState)
        return;

    mConfigRequestState = hwc_request_state_t::SET_CONFIG_STATE_NONE;
    mDesiredConfig = 0;
    uint64_t temp = 0;
    setPowerMode(HWC2_POWER_MODE_ON, temp);
    mActiveConfig = mDisplayInterface->getPreferredModeId();
    setActiveConfigInternal(mActiveConfig);
    setVsyncEnabledInternal(HWC2_VSYNC_DISABLE);
    mVsyncCallback.resetVsyncTimeStamp();
    mVsyncCallback.resetDesiredVsyncPeriod();
    mVsyncCallback.enableVSync(0);
    setPowerMode(HWC2_POWER_MODE_OFF, temp);
}

LayerDumpManager::LayerDumpManager(ExynosDisplay *display)
    : mDumpFrameIndex(0),
      mDumpMaxIndex(-1),
      mDisplay(display) {}

LayerDumpManager::~LayerDumpManager() {
    stop();
}

int32_t LayerDumpManager::getDumpFrameIndex() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mDumpFrameIndex;
}

int32_t LayerDumpManager::getDumpMaxIndex() {
    std::lock_guard<std::mutex> lock(mMutex);
    return mDumpMaxIndex;
}

layerDumpFrameInfo *LayerDumpManager::getLayerDumpFrameInfo(uint32_t idx) {
    std::lock_guard<std::mutex> lock(mMutex);
    return &mLayerDumpInfo[idx];
}

void LayerDumpManager::setCount(uint32_t cnt) {
    std::lock_guard<std::mutex> lock(mMutex);
    mDumpMaxIndex = cnt;
    mDumpFrameIndex = 0;
}

void LayerDumpManager::run(uint32_t cnt) {
    setCount(cnt);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mState = ThreadState::RUN;
    }
    if (!mThread.joinable())
        mThread = std::thread(&LayerDumpManager::loop, this);
    else
        ALOGI("LayerDumpManager::the thread is already started");
}

void LayerDumpManager::stop() {
    setCount(-1);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mState = ThreadState::STOPPED;
    }
    if (mThread.joinable()) {
        mThread.join();
        ALOGI("LayerDumpManager::stop the thread is joined");
    } else {
        ALOGE("LayerDumpManager::stop the thread join is impossible");
    }
}

bool LayerDumpManager::isRunning() {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mState == ThreadState::RUN)
        return true;
    else
        return false;
}

void LayerDumpManager::triggerDumpFrame() {
    std::lock_guard<std::mutex> lock(mMutex);

    if (mDumpFrameIndex > 0) {
        // wait previous buffer copy
        while (mLayerDumpInfo[mDumpFrameIndex].layerDumpState != LAYER_DUMP_DONE) {
            HDEBUGLOGD(eDebugHWC, "wait : %d, %d", mDumpFrameIndex, mState);
        }
    }
    mDumpFrameIndex++;
    mLayerDumpInfo[mDumpFrameIndex].layerDumpState = LAYER_DUMP_READY;
    HDEBUGLOGD(eDebugHWC, "%s trigger %d", __func__, mDumpFrameIndex);
}

void LayerDumpManager::loop() {
    while (true) {
        if (isRunning()) {
            mDisplay->getDumpLayer();
        } else {
            break;
        }
    }
    return;
}

hdrInterface *ExynosDisplay::createHdrInterfaceInstance() {
#ifdef USE_LIBHDR_PLUGIN
#ifdef HDR_IF_VER
    return hdrInterface::createInstance();
#else
    return hdrInterface::createInstance("/vendor/etc/dqe/HDR_coef_data_default.xml");
#endif
#else
    return nullptr;
#endif
}
hdr10pMetaInterface *ExynosDisplay::createHdr10PMetaInterfaceInstance() {
#ifdef USE_LIBHDR10P_META_PLUGIN
    return hdr10pMetaInterface::createInstance();
#else
    return nullptr;
#endif
}

int32_t ExynosDisplay::getDisplayMultiThreadedPresentSupport(bool &outSupport) {
    outSupport = mDisplayControl.multiThreadedPresent;
    return NO_ERROR;
}
