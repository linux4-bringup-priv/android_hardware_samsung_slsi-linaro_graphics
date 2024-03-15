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

/**
 * Project HWC 2.0 Design
 */
#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
#include <utils/Errors.h>
#include <android/sync.h>
#include <sys/mman.h>
#include <cutils/properties.h>
#include "ExynosMPP.h"
#include "ExynosResourceRestriction.h"
#include <hardware/hwcomposer_defs.h>
#include <math.h>
#include "ExynosGraphicBuffer.h"
#include "ExynosHWCDebug.h"
#include "ExynosHWCHelper.h"
#include "exynos_sync.h"

/**
 * ExynosMPP implementation
 *
 * Abstraction class for HW Resource
 */

using namespace android;
using namespace vendor::graphics;

int ExynosMPP::mainDisplayWidth = 0;
int ExynosMPP::mainDisplayHeight = 0;

ExynosFormat ExynosMPP::defaultMppDstFormat;
ExynosFormat ExynosMPP::defaultMppDstCompFormat;
ExynosFormat ExynosMPP::defaultMppDstYuvFormat;
ExynosFormat ExynosMPP::defaultMppDstUncompYuvFormat;

extern struct exynos_hwc_control exynosHWCControl;

void dumpExynosMPPImgInfo(uint32_t type, exynos_mpp_img_info &imgInfo) {
    HDEBUGLOGD(type, "\tbuffer: %p, bufferType: %d",
               imgInfo.bufferHandle, imgInfo.bufferType);
}

bool exynosMPPSourceComp(const ExynosMPPSource *l, const ExynosMPPSource *r) {
    if (l == NULL || r == NULL) {
        HWC_LOGE_NODISP("exynosMPP compare error");
        return 0;
    }
    return (l->mSrcImg.zOrder < r->mSrcImg.zOrder);
}

ExynosMPPSource::ExynosMPPSource(uint32_t sourceType, void *source)
    : mSourceType(sourceType),
      mSource(source),
      mOtfMPP(NULL),
      mM2mMPP(NULL) {
    mSrcImg.reset();
    mDstImg.reset();
    mMidImg.reset();
}

void ExynosMPPSource::setExynosImage(exynos_image &src_img, exynos_image &dst_img) {
    mSrcImg = src_img;
    mDstImg = dst_img;
}

void ExynosMPPSource::setExynosMidImage(exynos_image &mid_img) {
    mMidImg = mid_img;
}

void ExynosMPPSource::dump(String8 &str) {
    str.appendFormat("ImageDimension[%d, %d], src_rect[%d, %d, %d, %d], dst_rect[%d, %d, %d, %d], transform(0x%4x)",
                     mSrcImg.fullWidth, mSrcImg.fullHeight, mSrcImg.x, mSrcImg.y,
                     mSrcImg.x + mSrcImg.w, mSrcImg.y + mSrcImg.h, mMidImg.x, mMidImg.y,
                     mMidImg.x + mMidImg.w, mMidImg.y + mMidImg.h, mSrcImg.transform);
}

ExynosMPP::ExynosMPP(uint32_t physicalType, uint32_t logicalType, const char *name,
                     uint32_t physicalIndex, uint32_t logicalIndex,
                     uint32_t preAssignInfo, uint32_t mppType)
    : mMPPType(mppType),
      mPhysicalType(physicalType),
      mLogicalType(logicalType),
      mName(name),
      mPhysicalIndex(physicalIndex),
      mLogicalIndex(logicalIndex),
      mChId(-1),
      mPreAssignDisplayInfo(preAssignInfo),
      mHWState(MPP_HW_STATE_IDLE),
      mLastStateFenceFd(-1),
      mAssignedState(MPP_ASSIGN_STATE_FREE),
      mEnableByDebug(true),
      mDisableByUserScenario(0),
      mMaxSrcLayerNum(1),
      mPrevAssignedState(MPP_ASSIGN_STATE_FREE),
      mPrevAssignedDisplayType(-1),
      mResourceManageThread(this),
      mCapacity(-1),
      mUsedCapacity(0),
      mAllocOutBufFlag(true),
      mFreeOutBufFlag(true),
      mHWBusyFlag(false),
      mWasUsedPrevFrame(false),
      mCurrentDstBuf(0),
      mPrivDstBuf(-1),
      mTargetCompressionInfo({COMP_TYPE_NONE, 0, 0}),
      mCurrentTargetCompressionInfoType(COMP_TYPE_NONE),
      mAcrylicHandle(NULL),
      mUseM2MSrcFence(false),
      mAttr(0),
      mAssignOrder(0),
      mAXIPortId(0),
      mHWBlockId(0),
      mNeedSolidColorLayer(false) {
    for (int i = 0; i < RESTRICTION_MAX; i++) {
        mSrcSizeRestrictions[i] = {};
        mDstSizeRestrictions[i] = {};
    }

    if (mPhysicalType == MPP_G2D) {
        if (mLogicalType == MPP_LOGICAL_G2D_RGB) {
            char value[256];
            int afbc_prop;
            property_get("ro.vendor.ddk.set.afbc", value, "0");
            afbc_prop = atoi(value);
            if (afbc_prop == 0)
                mTargetCompressionInfo.type = COMP_TYPE_AFBC;
            else
                mTargetCompressionInfo.type = COMP_TYPE_NONE;

            mMaxSrcLayerNum = G2D_MAX_SRC_NUM;
        } else if (mLogicalType == MPP_LOGICAL_G2D_COMBO &&
                   (mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
            mMaxSrcLayerNum = G2D_MAX_SRC_NUM - 1;
            mAllocOutBufFlag = false;
            mTargetCompressionInfo.type = COMP_TYPE_NONE;
            mUseM2MSrcFence = true;
        }
        /* Capacity means time(ms) that can be used for operation */
        mCapacity = MPP_G2D_CAPACITY;
        mAcrylicHandle = AcrylicFactory::createAcrylic("default_compositor");
        if (mAcrylicHandle == NULL) {
            MPP_LOGE("Fail to allocate acrylic handle");
            abort();
        } else {
            MPP_LOGI("mAcrylicHandle is created: %p", mAcrylicHandle);
        }
    }

    if (mPhysicalType == MPP_MSC) {
        if (mLogicalType == MPP_LOGICAL_MSC_COMBO &&
            (mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
            mMaxSrcLayerNum = MSC_MAX_SRC_NUM;
            mAllocOutBufFlag = false;
            mTargetCompressionInfo.type = COMP_TYPE_NONE;
            mUseM2MSrcFence = true;
        }
        /* To do
        * Capacity should be set
        */
        mCapacity = MPP_MSC_CAPACITY;
        mAcrylicHandle = AcrylicFactory::createAcrylic("default_scaler");
        if (mAcrylicHandle == NULL) {
            MPP_LOGE("Fail to allocate acrylic handle");
            abort();
        } else {
            MPP_LOGI("mAcrylicHandle is created: %p", mAcrylicHandle);
        }
    }

    if (mMaxSrcLayerNum > 1) {
        mNeedSolidColorLayer = true;
        mAcrylicHandle->setDefaultColor(0, 0, 0, 0);
    }
    if (mMPPType == MPP_TYPE_OTF) {
        for (auto &chMap : IDMA_CHANNEL_MAP) {
            if ((chMap.type == mPhysicalType) && (chMap.index == mPhysicalIndex)) {
                mChId = static_cast<int32_t>(chMap.channel);
                break;
            }
        }
    }

    mAssignedSources.clear();
    resetUsedCapacity();

    mResourceManageThread.mRunning = true;
    mResourceManageThread.run();

    mPrevFrameInfo.reset();

    for (uint32_t i = 0; i < NUM_MPP_SRC_BUFS; i++) {
        mSrcImgs[i].reset();
    }
    for (uint32_t i = 0; i < NUM_MPP_DST_BUFS(mLogicalType); i++) {
        mDstImgs[i].reset();
    }

    for (uint32_t i = 0; i < DISPLAY_MODE_NUM; i++) {
        mPreAssignDisplayList[i] = (preAssignInfo >> (DISPLAY_MODE_MASK_LEN * i)) & DISPLAY_MODE_MASK_BIT;
    }
    mPreAssignedCapacity = (float)0.0f;
}

ExynosMPP::~ExynosMPP() {
    for (uint32_t i = 0; i < NUM_MPP_SRC_BUFS; i++) {
        if (mSrcImgs[i].mppLayer != NULL) {
            delete mSrcImgs[i].mppLayer;
            mSrcImgs[i].mppLayer = NULL;
        }
    }
    if (mAcrylicHandle != NULL)
        delete mAcrylicHandle;
    if ((mHdrCoefSize > 0) && mHdrCoefAddr) {
        munmap(mHdrCoefAddr, mHdrCoefSize);
    }
    if (mLutParcelFd >= 0)
        close(mLutParcelFd);
}

ExynosMPP::ResourceManageThread::ResourceManageThread(ExynosMPP *exynosMPP)
    : mExynosMPP(exynosMPP),
      mRunning(false) {
}

ExynosMPP::ResourceManageThread::~ResourceManageThread() {
    mRunning = false;
    mCondition.signal();
    if (mThread.joinable()) {
        mThread.join();
    }
}

void ExynosMPP::ResourceManageThread::run() {
    mThread = std::thread(&ResourceManageThread::threadLoop, this);
}

bool ExynosMPP::isDataspaceSupportedByMPP(struct exynos_image &src, struct exynos_image &dst) {
    bool ret = true;
    uint32_t srcStandard = (src.dataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t dstStandard = (dst.dataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t srcTransfer = (src.dataSpace & HAL_DATASPACE_TRANSFER_MASK);
    uint32_t dstTransfer = (dst.dataSpace & HAL_DATASPACE_TRANSFER_MASK);

    /* No conversion case */
    if ((srcStandard == dstStandard) && (srcTransfer == dstTransfer))
        return true;

    /* Unspecified conversion case */
    if (((srcStandard == HAL_DATASPACE_STANDARD_UNSPECIFIED) ||
         (dstStandard == HAL_DATASPACE_STANDARD_UNSPECIFIED)) &&
        ((srcTransfer == HAL_DATASPACE_TRANSFER_UNSPECIFIED) ||
         (dstTransfer == HAL_DATASPACE_TRANSFER_UNSPECIFIED)))
        return true;

    /* WCG support check */
    /* 'Src is not HDR' and 'src,dst has differenct dataspace' means WCG case */
    /* Some MPPs are only support HDR but WCG */
    if (!hasHdrInfo(src) && ((mAttr & MPP_ATTR_WCG) == 0))
        return false;

    /* Standard support check */
    if (dataspace_standard_map.find(srcStandard) != dataspace_standard_map.end()) {
        if (dataspace_standard_map.at(srcStandard).supported_hwc_attr & mAttr)
            ret = true;
        else
            return false;
    }

    /* Transfer support check */
    if (dataspace_transfer_map.find(srcTransfer) != dataspace_transfer_map.end()) {
        if (dataspace_transfer_map.at(srcTransfer).supported_hwc_attr & mAttr)
            ret = true;
        else
            return false;
    }

    if (ret)
        ret = checkCSCRestriction(src, dst);

    return ret;
}

bool ExynosMPP::isSupportedHDR10Plus(struct exynos_image &src, struct exynos_image &dst) {
    uint32_t srcStandard = (src.dataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t dstStandard = (dst.dataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t srcTransfer = (src.dataSpace & HAL_DATASPACE_TRANSFER_MASK);
    uint32_t dstTransfer = (dst.dataSpace & HAL_DATASPACE_TRANSFER_MASK);

    if (hasHdr10Plus(src)) {
        if (mAttr & MPP_ATTR_HDR10PLUS)
            return true;
        else if ((srcStandard == dstStandard) && (srcTransfer == dstTransfer))
            return true;
        else if ((mLogicalType == MPP_LOGICAL_G2D_COMBO) && (mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT))
            return true;
        else
            return false;
    }
    return true;
}

bool ExynosMPP::isSupportedHStrideCrop(struct exynos_image __unused &src) {
    return true;
}

bool ExynosMPP::isSupportedBlend(struct exynos_image &src) {
    switch (src.blending) {
    case HWC2_BLEND_MODE_NONE:
    case HWC2_BLEND_MODE_PREMULTIPLIED:
    case HWC2_BLEND_MODE_COVERAGE:
        return true;
    default:
        return false;
    }
}

bool ExynosMPP::checkRotationCondition(struct exynos_image &src) {
    /* Check only DPP types */
    if (mPhysicalType >= MPP_DPP_NUM)
        return true;

    /* If DPP has their own restriction, implmemnt module codes */
    bool isFlip = (((src.transform & HAL_TRANSFORM_FLIP_H) != 0) ||
                   ((src.transform & HAL_TRANSFORM_FLIP_V) != 0))
                      ? true
                      : false;

    bool isCompressed = (src.exynosFormat.isSBWC() ||
                         (src.compressionInfo.type == COMP_TYPE_SBWC) ||
                         (src.compressionInfo.type == COMP_TYPE_AFBC))
                            ? true
                            : false;

    bool isRotation = ((src.transform & HAL_TRANSFORM_ROT_90) != 0) ? true : false;

    if (mAttr & MPP_ATTR_ROT_90) {
        /* flip is not allowed for SBWC or AFBC*/
        if (src.exynosFormat.isYUV420()) {
            return (isCompressed && (isFlip && !isRotation)) ? false : true;
        }
    }

    if (isRotation) {
        return false;
    } else { /* flip is not allowed for SBWC or AFBC */
        return (isCompressed && isFlip) ? false : true;
    }
}

bool ExynosMPP::isSupportedTransform(struct exynos_image &src) {
    if (src.transform == 0)
        return true;

    /* If MPP need to check additional condition,
     * implement checkRotationCondition function to check it */
    /* For example, DPP need to check custom conditons */
    if (!checkRotationCondition(src))
        return false;

    for (auto transform_map : transform_map_table) {
        if (src.transform & transform_map.hal_tr) {
            if (!(mAttr & transform_map.hwc_tr))
                return false;
        }
    }

    return true;
}

bool ExynosMPP::isSupportedCompression(struct exynos_image &src) {
    if (src.compressionInfo.type == COMP_TYPE_AFBC) {
        if (mAttr & MPP_ATTR_AFBC)
            return true;
        else
            return false;
    }
    if (src.compressionInfo.type == COMP_TYPE_SAJC) {
        if (mAttr & MPP_ATTR_SAJC)
            return true;
        else
            return false;
    }
    return true;
}

bool ExynosMPP::isSharedMPPUsed() {
    /* Implement it to module */
    return false;
}

bool ExynosMPP::isSupportedCapability(DisplayInfo &display,
                                      struct exynos_image &src) {
    if (display.displayIdentifier.type != HWC_DISPLAY_EXTERNAL)
        return true;

    if (!(mAttr & MPP_ATTR_USE_CAPA))
        return true;

    bool hasHdrLayer =
        display.hdrLayersIndex.size() ? true : false;
    bool hasDrmLayer =
        display.drmLayersIndex.size() ? true : false;
    if (hasHdrLayer || hasDrmLayer) {
        if (getDrmMode(src.usageFlags) != NO_DRM)
            return true;
        else if (hasHdrInfo(src))
            return true;
        else
            return false;
    }

    return true;
}

bool ExynosMPP::isSupportedDRM(struct exynos_image &src) {
    if (getDrmMode(src.usageFlags) == NO_DRM)
        return true;

    if (mLogicalType == MPP_LOGICAL_G2D_RGB)
        return false;

    return true;
}

bool ExynosMPP::checkCSCRestriction(struct exynos_image &src, struct exynos_image &dst) {
    return true;
}

int32_t ExynosMPP::isDimLayerSupported() {
    if (mAttr & MPP_ATTR_DIM) {
        if (mLogicalType == MPP_LOGICAL_G2D_YUV)
            return -eMPPUnsupportedDIMLayer;
        return NO_ERROR;
    }

    return -eMPPUnsupportedDIMLayer;
}

bool ExynosMPP::isSrcFormatSupported(struct exynos_image &src) {
    if (mLogicalType == MPP_LOGICAL_G2D_YUV) {
        /* Support YUV layer and HDR RGB layer */
        if (src.exynosFormat.isRgb() && (hasHdrInfo(src) == false))
            return false;
    }
    if ((mLogicalType == MPP_LOGICAL_G2D_RGB) &&
        src.exynosFormat.isYUV())
        return false;
    if ((mLogicalType == MPP_LOGICAL_MSC_YUV) &&
        src.exynosFormat.isRgb()) {
        return false;
    }

    for (uint32_t i = 0; i < mFormatRestrictions.size(); i++) {
        if (((mFormatRestrictions[i].nodeType == NODE_NONE) ||
             (mFormatRestrictions[i].nodeType == NODE_SRC)) &&
            (mFormatRestrictions[i].format == src.exynosFormat.halFormat()))
            return true;
    }

    return false;
}

bool ExynosMPP::isDstFormatSupported(struct exynos_image &dst) {
    for (uint32_t i = 0; i < mFormatRestrictions.size(); i++) {
        if (((mFormatRestrictions[i].nodeType == NODE_NONE) ||
             (mFormatRestrictions[i].nodeType == NODE_DST)) &&
            (mFormatRestrictions[i].format == dst.exynosFormat.halFormat()))
            return true;
    }

    return false;
}

uint32_t ExynosMPP::getMaxUpscale(struct exynos_image &src, struct exynos_image __unused &dst) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxUpScale;
}

uint32_t ExynosMPP::getMaxDownscale(DisplayInfo &display,
                                    struct exynos_image &src, struct exynos_image &dst) {
    /* If SoC need to consider 'DPP performance' in determining whether scaliing is possible,
     * implement overrided function in SoC module */
    if ((mMPPType == MPP_TYPE_OTF) &&
        !scaleAllowedByDPPPerformance(display, src, dst)) {
        return 1;
    }

    uint32_t idx = getRestrictionClassification(src);

    return mDstSizeRestrictions[idx].maxDownScale;
}

bool ExynosMPP::scaleAllowedByDPPPerformance(DisplayInfo &display,
                                             struct exynos_image &src, struct exynos_image &dst) {
    bool isPerpendicular = !!(dst.transform & HAL_TRANSFORM_ROT_90);
    float scaleRatio_H = 1;
    float scaleRatio_V = 1;
    if (isPerpendicular) {
        scaleRatio_H = (float)src.w / (float)dst.h;
        scaleRatio_V = (float)src.h / (float)dst.w;
    } else {
        scaleRatio_H = (float)src.w / (float)dst.w;
        scaleRatio_V = (float)src.h / (float)dst.h;
    }

    float dstW = (float)dst.w;
    float displayW = (float)display.xres;
    float displayH = (float)display.yres;

    uint32_t vsyncPeriod = display.workingVsyncPeriod;
    int fps = (int)(1000000000 / vsyncPeriod);

    float vppResolClockFactor = fps * VPP_MARGIN;
    float resolClock = displayW * displayH * vppResolClockFactor;

    if (mAttr & MPP_ATTR_SCALE) {
        if (fps <= 60) {
            if ((float)VPP_CLOCK < ((resolClock * scaleRatio_H * scaleRatio_V * VPP_DISP_FACTOR) / VPP_PIXEL_PER_CLOCK * (dstW / displayW)))
                return false;
        } else {
            if ((float)VPP_CLOCK < ((resolClock * scaleRatio_H * scaleRatio_V * VPP_DISP_FACTOR) / VPP_PIXEL_PER_CLOCK))
                return false;
        }
    }
    return true;
}

uint32_t ExynosMPP::getSrcXOffsetAlign(struct exynos_image &src) {
    /* Refer module(ExynosMPPModule) for chip specific restrictions */
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].cropXAlign;
}
uint32_t ExynosMPP::getSrcXOffsetAlign(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].cropXAlign;
}
uint32_t ExynosMPP::getSrcYOffsetAlign(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].cropYAlign;
}
uint32_t ExynosMPP::getSrcYOffsetAlign(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].cropYAlign;
}
uint32_t ExynosMPP::getSrcWidthAlign(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].fullWidthAlign;
}
uint32_t ExynosMPP::getSrcHeightAlign(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].fullHeightAlign;
}
uint32_t ExynosMPP::getSrcMaxWidth(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxFullWidth;
}
uint32_t ExynosMPP::getSrcMaxHeight(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxFullHeight;
}
uint32_t ExynosMPP::getSrcMinWidth(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].minFullWidth;
}
uint32_t ExynosMPP::getSrcMinWidth(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].minFullWidth;
}
uint32_t ExynosMPP::getSrcMinHeight(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].minFullHeight;
}
uint32_t ExynosMPP::getSrcMinHeight(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].minFullHeight;
}
uint32_t ExynosMPP::getSrcMaxCropWidth(struct exynos_image &src) {
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxCropWidth;
}
uint32_t ExynosMPP::getSrcMaxCropHeight(struct exynos_image &src) {
    if ((mMPPType == MPP_TYPE_OTF) &&
        (src.transform & HAL_TRANSFORM_ROT_90))
        return 2160;

    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxCropHeight;
}
uint32_t ExynosMPP::getSrcMaxCropSize(struct exynos_image &src) {
    return (getSrcMaxCropWidth(src) * getSrcMaxCropHeight(src));
}
uint32_t ExynosMPP::getSrcMinCropWidth(struct exynos_image &src) {
    if (((src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 2;
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].minCropWidth;
}
uint32_t ExynosMPP::getSrcMinCropHeight(struct exynos_image &src) {
    if (((src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 2;
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].minCropHeight;
}
uint32_t ExynosMPP::getSrcCropWidthAlign(struct exynos_image &src) {
    if (((src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 2;
    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].cropWidthAlign;
}

/* This is used for only otfMPP */
uint32_t ExynosMPP::getSrcCropWidthAlign(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].cropWidthAlign;
}
uint32_t ExynosMPP::getSrcCropHeightAlign(struct exynos_image &src) {
    if (((src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 2;

    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].cropHeightAlign;
}

/* This is used for only otfMPP */
uint32_t ExynosMPP::getSrcCropHeightAlign(uint32_t idx) {
    if (idx >= RESTRICTION_MAX) {
        MPP_LOGE("invalid idx: %d", idx);
        return 16;
    }
    return mSrcSizeRestrictions[idx].cropHeightAlign;
}
uint32_t ExynosMPP::getDstMaxWidth(struct exynos_image &dst) {
    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].maxCropWidth;
}
uint32_t ExynosMPP::getDstMaxHeight(struct exynos_image &dst) {
    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].maxCropHeight;
}
uint32_t ExynosMPP::getDstMinWidth(struct exynos_image &dst) {
    if (((dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 64;

    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 32;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].minCropWidth;
}
uint32_t ExynosMPP::getDstMinHeight(struct exynos_image &dst) {
    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 8;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].minCropHeight;
}
uint32_t ExynosMPP::getDstWidthAlign(struct exynos_image &dst) {
    if (((dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_G2D))
        return 64;

    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 32;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].cropWidthAlign;
}
uint32_t ExynosMPP::getDstHeightAlign(struct exynos_image &dst) {
    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 8;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].cropHeightAlign;
}
uint32_t ExynosMPP::getDstXOffsetAlign(struct exynos_image &dst) {
    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 32;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].cropXAlign;
}
uint32_t ExynosMPP::getDstYOffsetAlign(struct exynos_image &dst) {
    if ((mNeedSolidColorLayer == false) && mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;

    if ((mPhysicalType == MPP_G2D) && (mNeedSolidColorLayer == false) &&
        dst.exynosFormat.isSBWC())
        return 8;

    uint32_t idx = getRestrictionClassification(dst);
    return mDstSizeRestrictions[idx].cropYAlign;
}
uint32_t ExynosMPP::getOutBufAlign() {
    if (mTargetCompressionInfo.type == COMP_TYPE_AFBC)
        return 16;
    else
        return 1;
}

int32_t ExynosMPP::isSupportLayerColorTransform(
    struct exynos_image &src, struct exynos_image __unused &dst) {
    if (src.needColorTransform == false)
        return true;

    if (mAttr & MPP_ATTR_LAYER_TRANSFORM)
        return true;

    return false;
}

void ExynosMPP::ResourceManageThread::threadLoop() {
    if (mExynosMPP == NULL)
        return;

    ALOGI("%s threadLoop is started", mExynosMPP->mName.c_str());
    while (mRunning) {
        Mutex::Autolock lock(mMutex);
        while ((mFreedBuffers.size() == 0) &&
               (mStateFences.size() == 0) &&
               (mRunning == true)) {
            mCondition.wait(mMutex);
        }

        if ((mExynosMPP->mHWState == MPP_HW_STATE_RUNNING) &&
            (mStateFences.size() != 0)) {
            if (checkStateFences()) {
                mExynosMPP->mHWState = MPP_HW_STATE_IDLE;
            }
        } else {
            if ((mStateFences.size() != 0) &&
                (mExynosMPP->mHWState != MPP_HW_STATE_RUNNING)) {
                ALOGW("%s, mHWState(%d) but mStateFences size(%zu)",
                      mExynosMPP->mName.c_str(), mExynosMPP->mHWState,
                      mStateFences.size());
                checkStateFences();
            }
        }

        if (mFreedBuffers.size() != 0) {
            freeBuffers();
        }
    }
    ALOGI("%s threadLoop is ended", mExynosMPP->mName.c_str());
}

void ExynosMPP::ResourceManageThread::freeBuffers() {
    ExynosGraphicBufferAllocator &gAllocator(ExynosGraphicBufferAllocator::get());
    android::List<exynos_mpp_img_info>::iterator it;
    android::List<exynos_mpp_img_info>::iterator end;
    it = mFreedBuffers.begin();
    end = mFreedBuffers.end();

    uint32_t freebufNum = 0;
    while (it != end) {
        exynos_mpp_img_info freeBuffer = (exynos_mpp_img_info)(*it);
        HDEBUGLOGD(eDebugMPP | eDebugBuf, "freebufNum: %d, buffer: %p", freebufNum, freeBuffer.bufferHandle);
        dumpExynosMPPImgInfo(eDebugMPP | eDebugBuf, freeBuffer);
        if (mExynosMPP->mFenceTracer.fence_valid(freeBuffer.acrylicAcquireFenceFd)) {
            // mExynosMPP->mAssignedDisplay can be null
            freeBuffer.acrylicAcquireFenceFd =
                mExynosMPP->mFenceTracer.fence_close(freeBuffer.acrylicAcquireFenceFd,
                                                     mExynosMPP->mAssignedDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL,
                                                     "mpp::freeBuffers: acrylicAcquireFence");
        }
        if (mExynosMPP->mFenceTracer.fence_valid(freeBuffer.acrylicReleaseFenceFd)) {
            // mExynosMPP->mAssignedDisplay can be null
            freeBuffer.acrylicReleaseFenceFd =
                mExynosMPP->mFenceTracer.fence_close(freeBuffer.acrylicReleaseFenceFd,
                                                     mExynosMPP->mAssignedDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_SRC_RELEASE, FENCE_IP_ALL,
                                                     "mpp::freeBuffers: acrylicReleaseFence");
        }
        gAllocator.free(freeBuffer.bufferHandle);
        it = mFreedBuffers.erase(it);
    }
}

bool ExynosMPP::ResourceManageThread::checkStateFences() {
    bool ret = true;
    android::List<int>::iterator it;
    android::List<int>::iterator end;

    it = mStateFences.begin();
    end = mStateFences.end();
    uint32_t waitFenceNum = 0;
    while (it != end) {
        int fence = (int)(*it);
        HDEBUGLOGD(eDebugMPP, "%d wait fence: %d", waitFenceNum, fence);
        waitFenceNum++;
        if (mExynosMPP->mFenceTracer.fence_valid(fence)) {
            if (sync_wait(fence, 5000) < 0) {
                HWC_LOGE_NODISP("%s::[%s][%d] sync_wait(%d) error(%s)", __func__,
                                mExynosMPP->mName.c_str(), mExynosMPP->mLogicalIndex, fence, strerror(errno));
                ret = false;
            }
            // mExynosMPP->mAssignedDisplay can be null
            fence = mExynosMPP->mFenceTracer.fence_close(fence,
                                                         mExynosMPP->mAssignedDisplayInfo.displayIdentifier,
                                                         FENCE_TYPE_UNDEFINED, FENCE_IP_ALL,
                                                         "mpp::checkStateFences:: hw state fence");
        }
        it = mStateFences.erase(it);
    }
    return ret;
}

void ExynosMPP::ResourceManageThread::addFreedBuffer(exynos_mpp_img_info freedBuffer) {
    Mutex::Autolock lock(mMutex);
    mFreedBuffers.push_back(freedBuffer);
    mCondition.signal();
}

void ExynosMPP::ResourceManageThread::addStateFence(int fence) {
    Mutex::Autolock lock(mMutex);
    HDEBUGLOGD(eDebugMPP, "wait fence is added: %d", fence);
    mStateFences.push_back(fence);
    mCondition.signal();
}

/**
 * @param w
 * @param h
 * @param color
 * @param usage
 * @return int32_t
 */
int32_t ExynosMPP::allocOutBuf(uint32_t w, uint32_t h, uint32_t format, uint64_t usage, uint32_t index) {
    ATRACE_CALL();
    uint32_t dstStride = 0;

    MPP_LOGD(eDebugMPP | eDebugBuf, "index: %d++++++++", index);

    if (index >= NUM_MPP_DST_BUFS(mLogicalType)) {
        return -EINVAL;
    }

    exynos_mpp_img_info freeDstBuf = mDstImgs[index];
    MPP_LOGD(eDebugMPP | eDebugBuf, "mDstImg[%d] is reallocated", index);
    dumpExynosMPPImgInfo(eDebugMPP, mDstImgs[index]);

    uint64_t allocUsage = getBufferUsage(usage);
    buffer_handle_t dstBuffer;

    MPP_LOGD(eDebugMPP | eDebugBuf, "\tw: %d, h: %d, format: 0x%8x, previousBuffer: %p, allocUsage: 0x%" PRIx64 ", usage: 0x%" PRIx64 "",
             w, h, format, freeDstBuf.bufferHandle, allocUsage, usage);

    status_t error = NO_ERROR;
    ExynosGraphicBufferAllocator &gAllocator(ExynosGraphicBufferAllocator::get());

    {
        ATRACE_CALL();
        error = gAllocator.allocate(w, h, format, 1, allocUsage, &dstBuffer, &dstStride, "HWC");
    }

    bool freeBuffer = false;
    if ((error != NO_ERROR) || (dstBuffer == NULL)) {
        MPP_LOGW("failed to allocate destination buffer(%dx%d): %d", w, h, error);

        if (freeDstBuf.bufferHandle != NULL) {
            MPP_LOGW("retry to allocate destination buffer after free buffer");

            auto freeOutBuf = [&]() -> void {
                dumpExynosMPPImgInfo(eDebugMPP | eDebugBuf, freeDstBuf);
                if (mFenceTracer.fence_valid(freeDstBuf.acrylicAcquireFenceFd)) {
                    freeDstBuf.acrylicAcquireFenceFd =
                        mFenceTracer.fence_close(freeDstBuf.acrylicAcquireFenceFd,
                                                 mAssignedDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_ALL,
                                                 "mpp::freeBuffers: acrylicAcquireFence");
                }
                if (mFenceTracer.fence_valid(freeDstBuf.acrylicReleaseFenceFd)) {
                    freeDstBuf.acrylicReleaseFenceFd =
                        mFenceTracer.fence_close(freeDstBuf.acrylicReleaseFenceFd,
                                                 mAssignedDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_RELEASE, FENCE_IP_ALL,
                                                 "mpp::freeBuffers: acrylicReleaseFence");
                }
                gAllocator.free(freeDstBuf.bufferHandle);
                freeDstBuf.reset();
                freeBuffer = true;
                ALOGW("free buffer: %p", freeDstBuf.bufferHandle);
            };

            {
                ATRACE_CALL();
                freeOutBuf();
                error = gAllocator.allocate(w, h, format, 1, allocUsage, &dstBuffer, &dstStride, "HWC");
            }
            if ((error != NO_ERROR) || (dstBuffer == NULL)) {
                MPP_LOGE("retry is failed to allocate destination buffer(%dx%d): %d", w, h, error);
                return -EINVAL;
            }
        }
    }

    mDstImgs[index].reset();
    mDstImgs[index].bufferHandle = dstBuffer;
    mDstImgs[index].bufferType = getBufferType(usage);
    mDstImgs[index].format = format;

    if (!freeBuffer) {
        MPP_LOGD(eDebugMPP | eDebugBuf, "free outbuf[%d] %p", index, freeDstBuf.bufferHandle);
        if (freeDstBuf.bufferHandle != NULL)
            freeOutBuf(freeDstBuf);
        else {
            if (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) {
                freeDstBuf.acrylicAcquireFenceFd = mFenceTracer.fence_close(
                        freeDstBuf.acrylicAcquireFenceFd, mAssignedDisplayInfo.displayIdentifier,
                        FENCE_TYPE_DST_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
                        "mpp::allocOutBuf: acrylicAcquireFence");
                freeDstBuf.acrylicReleaseFenceFd = mFenceTracer.fence_close(
                        freeDstBuf.acrylicReleaseFenceFd, mAssignedDisplayInfo.displayIdentifier,
                        FENCE_TYPE_DST_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
                        "mpp::allocOutBuf: acrylicReleaseFence");
            }
        }
    }

    MPP_LOGD(eDebugMPP | eDebugBuf, "dstBuffer(%p)-----------", dstBuffer);

    return NO_ERROR;
}

/**
 * @param outbuf
 * @return int32_t
 */
int32_t ExynosMPP::setOutBuf(buffer_handle_t outbuf, int32_t fence,
                             DisplayInfo &display) {
    mDstImgs[mCurrentDstBuf].bufferHandle = NULL;
    if (outbuf != NULL) {
        mDstImgs[mCurrentDstBuf].bufferHandle = outbuf;
        mDstImgs[mCurrentDstBuf].format =
            ExynosGraphicBufferMeta::get_format(mDstImgs[mCurrentDstBuf].bufferHandle);
    }
    setDstReleaseFence(fence, display);
    return NO_ERROR;
}

/**
 * @param dst
 * @return int32_t
 */
int32_t ExynosMPP::freeOutBuf(struct exynos_mpp_img_info dst) {
    if (mBufDestoryedCallback)
        mBufDestoryedCallback(ExynosGraphicBufferMeta::get_buffer_id(dst.bufferHandle));
    mResourceManageThread.addFreedBuffer(dst);
    dst.bufferHandle = NULL;
    return NO_ERROR;
}

uint32_t ExynosMPP::getBufferType(uint64_t usage) {
    if (getDrmMode(usage) == SECURE_DRM)
        return MPP_BUFFER_SECURE_DRM;
    else if (getDrmMode(usage) == NORMAL_DRM)
        return MPP_BUFFER_NORMAL_DRM;
    else {
        if (exynosHWCControl.dumpMidBuf)
            return MPP_BUFFER_DUMP;
        else
            return MPP_BUFFER_NORMAL;
    }
}

uint32_t ExynosMPP::getBufferType(const buffer_handle_t handle) {
#ifdef GRALLOC_VERSION1
    uint64_t usage = ExynosGraphicBufferMeta::get_usage(handle);
#else
    uint64_t usage = handle->flags;
#endif
    return getBufferType(usage);
}

uint64_t ExynosMPP::getBufferUsage(uint64_t usage) {
    uint64_t allocUsage = 0;
#ifdef GRALLOC_VERSION1
    if (getBufferType(usage) == MPP_BUFFER_DUMP) {
        allocUsage = BufferUsage::CPU_READ_OFTEN |
                     BufferUsage::CPU_WRITE_OFTEN;
    } else {
        allocUsage = BufferUsage::CPU_READ_NEVER |
                     BufferUsage::CPU_WRITE_NEVER |
                     ExynosGraphicBufferUsage::NOZEROED |
                     BufferUsage::COMPOSER_OVERLAY;
    }

    if (getDrmMode(usage) == SECURE_DRM) {
        allocUsage |= BufferUsage::PROTECTED;
        allocUsage &= ~ExynosGraphicBufferUsage::PRIVATE_NONSECURE;
    } else if (getDrmMode(usage) == NORMAL_DRM) {
        allocUsage |= BufferUsage::PROTECTED;
        allocUsage |= ExynosGraphicBufferUsage::PRIVATE_NONSECURE;
    }

#else
    if (getBufferType(usage) == MPP_BUFFER_DUMP) {
        allocUsage = BufferUsage::SW_READ_OFTEN |
                     BufferUsage::SW_WRITE_OFTEN;
    } else {
        allocUsage = BufferUsage::SW_READ_NEVER |
                     BufferUsage::SW_WRITE_NEVER |
                     BufferUsage::NOZEROED |
                     BufferUsage::HW_COMPOSER;
    }

    if (getDrmMode(usage) == SECURE_DRM) {
        allocUsage |= BufferUsage::PROTECTED;
        allocUsage &= ~BufferUsage::PRIVATE_NONSECURE;
    } else if (getDrmMode(usage) == NORMAL_DRM) {
        allocUsage |= BufferUsage::PROTECTED;
        allocUsage |= BufferUsage::PRIVATE_NONSECURE;
    }

    /* HACK: for distinguishing FIMD_VIDEO_region */
    if (!((allocUsage & BufferUsage::PROTECTED) &&
          !(allocUsage & BufferUsage::PRIVATE_NONSECURE) &&
          !(allocUsage & BufferUsage::VIDEO_EXT))) {
        allocUsage |= (BufferUsage::HW_TEXTURE | BufferUsage::HW_RENDER);
    }
#endif

    return allocUsage;
}

bool ExynosMPP::needDstBufRealloc(struct exynos_image &dst, uint32_t index) {
    MPP_LOGD(eDebugMPP | eDebugBuf, "index: %d++++++++", index);

    if (index >= NUM_MPP_DST_BUFS(mLogicalType)) {
        MPP_LOGE("%s:: index(%d) is not valid", __func__, index);
        return false;
    }
    buffer_handle_t dst_handle = NULL;
    if (mDstImgs[index].bufferHandle != NULL)
        dst_handle = mDstImgs[index].bufferHandle;

    if (dst_handle == NULL) {
        MPP_LOGD(eDebugMPP | eDebugBuf, "\tDstImag[%d]  handle is NULL", index);
        return true;
    }

    int32_t assignedDisplay = -1;
    if (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) {
        assignedDisplay = mAssignedDisplayInfo.displayIdentifier.type;
    } else {
        MPP_LOGE("%s:: mpp is not assigned", __func__);
        return false;
    }

    ExynosGraphicBufferMeta gmeta(dst_handle);

    MPP_LOGD(eDebugMPP | eDebugBuf, "\tdst_handle(%p)", dst_handle);
    MPP_LOGD(eDebugMPP | eDebugBuf, "\tAssignedDisplay[%d, %d] format[0x%8x, %s], bufferType[%d, %d], usageFlags: 0x%" PRIx64 "",
             mPrevAssignedDisplayType, assignedDisplay, gmeta.format, dst.exynosFormat.name().c_str(),
             mDstImgs[index].bufferType, getBufferType(dst.usageFlags), dst.usageFlags);

    bool realloc = (mPrevAssignedDisplayType != assignedDisplay) ||
                   (formatToBpp(gmeta.format) < dst.exynosFormat.bpp()) ||
                   ((gmeta.stride * gmeta.vstride) < (int)(dst.fullWidth * dst.fullHeight)) ||
                   ((!isFormatSBWC(gmeta.format)) && dst.exynosFormat.isSBWC()) ||
                   (mDstImgs[index].bufferType != getBufferType(dst.usageFlags));

    MPP_LOGD(eDebugMPP | eDebugBuf, "realloc: %d--------", realloc);
    return realloc;
}

bool ExynosMPP::canUsePrevFrame(struct exynos_image &src) {
    if (canUseVotf(src))
        return false;

    if (mAssignedDisplayInfo.skipM2mProcessing == false)
        return false;

    /* virtual display always require composition */
    if (mAllocOutBufFlag == false)
        return false;

    if (mPrevFrameInfo.srcNum != mAssignedSources.size())
        return false;

    for (uint32_t i = 0; i < mPrevFrameInfo.srcNum; i++) {
        if ((mPrevFrameInfo.srcInfo[i].bufferHandle != mAssignedSources[i]->mSrcImg.bufferHandle) ||
            (mPrevFrameInfo.srcInfo[i].x != mAssignedSources[i]->mSrcImg.x) ||
            (mPrevFrameInfo.srcInfo[i].y != mAssignedSources[i]->mSrcImg.y) ||
            (mPrevFrameInfo.srcInfo[i].w != mAssignedSources[i]->mSrcImg.w) ||
            (mPrevFrameInfo.srcInfo[i].h != mAssignedSources[i]->mSrcImg.h) ||
            (mPrevFrameInfo.srcInfo[i].exynosFormat != mAssignedSources[i]->mSrcImg.exynosFormat) ||
            (mPrevFrameInfo.srcInfo[i].usageFlags != mAssignedSources[i]->mSrcImg.usageFlags) ||
            (mPrevFrameInfo.srcInfo[i].dataSpace != mAssignedSources[i]->mSrcImg.dataSpace) ||
            (mPrevFrameInfo.srcInfo[i].blending != mAssignedSources[i]->mSrcImg.blending) ||
            (mPrevFrameInfo.srcInfo[i].transform != mAssignedSources[i]->mSrcImg.transform) ||
            (mPrevFrameInfo.srcInfo[i].compressionInfo.type != mAssignedSources[i]->mSrcImg.compressionInfo.type) ||
            (mPrevFrameInfo.srcInfo[i].planeAlpha != mAssignedSources[i]->mSrcImg.planeAlpha) ||
            (mPrevFrameInfo.srcInfo[i].layerFlags != mAssignedSources[i]->mSrcImg.layerFlags) ||
            (mPrevFrameInfo.srcInfo[i].usageFlags != mAssignedSources[i]->mSrcImg.usageFlags) ||
            (mPrevFrameInfo.srcInfo[i].color.r != mAssignedSources[i]->mSrcImg.color.r) ||
            (mPrevFrameInfo.srcInfo[i].color.g != mAssignedSources[i]->mSrcImg.color.g) ||
            (mPrevFrameInfo.srcInfo[i].color.b != mAssignedSources[i]->mSrcImg.color.b) ||
            (mPrevFrameInfo.srcInfo[i].color.a != mAssignedSources[i]->mSrcImg.color.a) ||
            (mPrevFrameInfo.dstInfo[i].x != mAssignedSources[i]->mMidImg.x) ||
            (mPrevFrameInfo.dstInfo[i].y != mAssignedSources[i]->mMidImg.y) ||
            (mPrevFrameInfo.dstInfo[i].w != mAssignedSources[i]->mMidImg.w) ||
            (mPrevFrameInfo.dstInfo[i].h != mAssignedSources[i]->mMidImg.h) ||
            (mPrevFrameInfo.dstInfo[i].exynosFormat != mAssignedSources[i]->mMidImg.exynosFormat))
            return false;
    }

    int32_t prevDstIndex = (mCurrentDstBuf + NUM_MPP_DST_BUFS(mLogicalType) - 1) % NUM_MPP_DST_BUFS(mLogicalType);
    if (mDstImgs[prevDstIndex].bufferHandle == NULL)
        return false;

    return true;
}

int32_t ExynosMPP::enableVotfInfo(VotfInfo &info) {
    if (mMPPType != MPP_TYPE_OTF)
        return -EINVAL;

    mVotfInfo.enable = true;
    mVotfInfo.bufIndex++;
    if (mVotfInfo.bufIndex >= VOTF_BUF_INDEX_MAX)
        mVotfInfo.bufIndex = 0;
    info = mVotfInfo;

    return NO_ERROR;
}

int32_t ExynosMPP::setVotfInfo(const VotfInfo &info) {
    if (mMPPType != MPP_TYPE_M2M)
        return -EINVAL;

    mVotfInfo = info;
    return NO_ERROR;
}

int32_t ExynosMPP::setupLayer(exynos_mpp_img_info *srcImgInfo, struct exynos_image &src, struct exynos_image &dst) {
    int ret = NO_ERROR;

    if (srcImgInfo->mppLayer == NULL) {
        if ((srcImgInfo->mppLayer = mAcrylicHandle->createLayer()) == NULL) {
            MPP_LOGE("%s:: Fail to create layer", __func__);
            return -EINVAL;
        }
    }

    if (src.layerFlags & EXYNOS_HWC_DIM_LAYER) {
        MPP_LOGD(eDebugMPP, "%s:: setup dim layer", __func__);
    } else if (src.bufferHandle == NULL) {
        MPP_LOGE("%s:: Invalid source handle", __func__);
        return -EINVAL;
    }

    buffer_handle_t srcHandle = NULL;
    if (src.bufferHandle != NULL)
        srcHandle = src.bufferHandle;
    int bufFds[MAX_HW2D_PLANES];
    size_t bufLength[MAX_HW2D_PLANES] = {0};
    uint32_t attribute = 0;
    auto formatDesc = src.exynosFormat.getFormatDesc();
    uint32_t bufferNum = formatDesc.bufferNum;
    android_dataspace_t dataspace = src.dataSpace;

    if (bufferNum == 0) {
        MPP_LOGE("%s:: Fail to get bufferNum(%d), format(%s)", __func__, bufferNum, formatDesc.name.c_str());
        return -EINVAL;
    }

    for (int i = 0; i < MAX_HW2D_PLANES; i++)
        bufFds[i] = -1;

    dataspace = getRefinedDataspace(src.exynosFormat.halFormat(), dataspace);

    if (!(src.layerFlags & EXYNOS_HWC_DIM_LAYER)) {
        ExynosGraphicBufferMeta gmeta(srcHandle);

        bufFds[0] = gmeta.fd;
        bufFds[1] = gmeta.fd1;
        bufFds[2] = gmeta.fd2;
        if (getBufLength(srcHandle, MAX_HW2D_PLANES, bufLength, formatDesc.halFormat, src.fullWidth, src.fullHeight) != NO_ERROR) {
            MPP_LOGE("%s:: invalid bufferLength(%zu, %zu, %zu), format(%s)", __func__,
                     bufLength[0], bufLength[1], bufLength[2], src.exynosFormat.name().c_str());
            return -EINVAL;
        }

        /* HDR process */
        if (hasHdrInfo(src) && src.metaParcel) {
            unsigned int min = src.metaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance;
            unsigned int max = (src.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance / 10000);

            srcImgInfo->mppLayer->setMasterDisplayLuminance(min, max);
            MPP_LOGD(eDebugMPP, "HWC2: G2D luminance min %d, max %d", min, max);
            MPP_LOGD(eDebugMPP, "G2D getting HDR source!");
        }

        /* Transfer MetaData */
        if (src.metaParcel) {
            srcImgInfo->mppLayer->setLayerData(src.metaParcel, sizeof(ExynosVideoMeta));
        }

        setVotfLayerData(srcImgInfo);

        srcImgInfo->bufferType = getBufferType(srcHandle);
        if (srcImgInfo->bufferType == MPP_BUFFER_SECURE_DRM)
            attribute |= AcrylicCanvas::ATTR_PROTECTED;
        if (src.compressionInfo.type == COMP_TYPE_AFBC)
            attribute |= AcrylicCanvas::ATTR_COMPRESSED;
    }

    srcImgInfo->bufferHandle = srcHandle;
    srcImgInfo->acrylicAcquireFenceFd =
        mFenceTracer.checkFenceDebug(mAssignedDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType), src.acquireFenceFd);

    MPP_LOGD(eDebugMPP, "source configuration:");
    MPP_LOGD(eDebugMPP, "\tImageDimension[%d, %d], ImageType[%s, 0x%8x]",
             src.fullWidth, src.fullHeight,
             src.exynosFormat.name().c_str(), dataspace);
    MPP_LOGD(eDebugMPP, "\tImageBuffer handle: %p, fds[%d, %d, %d], bufLength[%zu, %zu, %zu], bufferNum: %d, acquireFence: %d, attribute: %d",
             srcHandle, bufFds[0], bufFds[1], bufFds[2], bufLength[0], bufLength[1], bufLength[2],
             bufferNum, srcImgInfo->acrylicAcquireFenceFd, attribute);
    MPP_LOGD(eDebugMPP, "\tsrc_rect[%d, %d, %d, %d], dst_rect[%d, %d, %d, %d], transform(0x%4x)",
             (int)src.x, (int)src.y, (int)(src.x + src.w), (int)(src.y + src.h),
             (int)dst.x, (int)dst.y, (int)(dst.x + dst.w), (int)(dst.y + dst.h), src.transform);

    srcImgInfo->mppLayer->setImageDimension(src.fullWidth, src.fullHeight);

    if (formatDesc.halFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV) {
        srcImgInfo->mppLayer->setImageType(HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M, dataspace);
    } else {
        srcImgInfo->mppLayer->setImageType(formatDesc.halFormat, dataspace);
    }

    mFenceTracer.setFenceInfo(srcImgInfo->acrylicAcquireFenceFd,
                              mAssignedDisplayInfo.displayIdentifier,
                              FENCE_TYPE_SRC_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType), FENCE_TO);

    if (src.layerFlags & EXYNOS_HWC_DIM_LAYER) {
        srcImgInfo->mppLayer->setImageBuffer(src.color.a, src.color.r, src.color.g, src.color.b, 0);
    } else {
        srcImgInfo->mppLayer->setImageBuffer(bufFds, bufLength, bufferNum,
                                             srcImgInfo->acrylicAcquireFenceFd, attribute);
    }

    if (mMaxSrcLayerNum > 1) {
        srcImgInfo->mppLayer->setCompositMode(src.blending, (uint8_t)(255 * src.planeAlpha), src.zOrder);
    } else {
        srcImgInfo->mppLayer->setCompositMode(src.blending, 255, src.zOrder);
    }

    hwc_rect_t src_rect = {(int)src.x, (int)src.y, (int)(src.x + src.w), (int)(src.y + src.h)};
    hwc_rect_t dst_rect = {(int)dst.x, (int)dst.y, (int)(dst.x + dst.w), (int)(dst.y + dst.h)};

    if ((mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
        ((mAssignedDisplayInfo.displayIdentifier.type == HWC_DISPLAY_VIRTUAL) ||
         (mAssignedDisplayInfo.displayIdentifier.type == HWC_DISPLAY_EXTERNAL)))
        srcImgInfo->mppLayer->setCompositArea(src_rect, dst_rect, src.transform, AcrylicLayer::ATTR_NORESAMPLING);
    else {
        if (src.exynosFormat.isYUV())
            srcImgInfo->mppLayer->setCompositArea(src_rect, dst_rect, src.transform, AcrylicLayer::ATTR_NORESAMPLING);
        else
            srcImgInfo->mppLayer->setCompositArea(src_rect, dst_rect, src.transform);
    }

    srcImgInfo->acrylicAcquireFenceFd = -1;
    srcImgInfo->format = formatDesc.halFormat;

    if (formatDesc.halFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV) {
        srcImgInfo->format = HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M;
    }

    return ret;
}

android_dataspace_t ExynosMPP::getDstDataspace(int dstFormat,
                                               DisplayInfo &display, android_dataspace_t dstDataspace) {
    bool isComposition = (mMaxSrcLayerNum > 1);
    android_dataspace_t dataspace = HAL_DATASPACE_UNKNOWN;
    if (isComposition) {
        if (isFormatRgb(dstFormat)) {
            if (display.colorMode != HAL_COLOR_MODE_NATIVE)
                dataspace = colorModeToDataspace(display.colorMode);
        } else {
            dataspace =
                (android_dataspace)(HAL_DATASPACE_STANDARD_BT709 | HAL_DATASPACE_TRANSFER_GAMMA2_2 | HAL_DATASPACE_RANGE_LIMITED);
        }
    } else {
        if (mAssignedSources.size() > 0)
            dataspace = mAssignedSources[0]->mMidImg.dataSpace;
        else
            dataspace = dstDataspace;
    }

    dataspace = getRefinedDataspace(dstFormat, dataspace);

    return dataspace;
}

dstMetaInfo_t ExynosMPP::getDstMetaInfo(android_dataspace_t dstDataspace) {
    dstMetaInfo_t metaInfo;

    if ((mAssignedSources.size() <= 1) &&
        (mAssignedSources[0]->mSrcImg.dataSpace == dstDataspace) &&
        (mAssignedSources[0]->mSrcImg.metaParcel != nullptr)) {
        metaInfo.minLuminance =
            (uint16_t)mAssignedSources[0]->mSrcImg.metaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance;
        metaInfo.maxLuminance =
            (uint16_t)(mAssignedSources[0]->mSrcImg.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance / 10000);
    } else {
        // minLuminance: 0.0001nit unit, maxLuminance: 1nit unit
        metaInfo.minLuminance = (uint16_t)(mAssignedDisplayInfo.minLuminance * 10000);
        metaInfo.maxLuminance = (uint16_t)mAssignedDisplayInfo.maxLuminance;
    }

    return metaInfo;
}

int32_t ExynosMPP::setupDst(exynos_mpp_img_info *dstImgInfo) {
    int ret = NO_ERROR;
    bool isComposition = (mMaxSrcLayerNum > 1);
    buffer_handle_t dstHandle = dstImgInfo->bufferHandle;
    int bufFds[MAX_HW2D_PLANES];
    size_t bufLength[MAX_HW2D_PLANES];
    uint32_t attribute = 0;
    auto formatDesc = dstImgInfo->format.getFormatDesc();
    uint32_t bufferNum = formatDesc.bufferNum;
    if (bufferNum == 0) {
        MPP_LOGE("%s:: Fail to get bufferNum(%d), format(%s)", __func__,
                 bufferNum, dstImgInfo->format.name().c_str());
        return -EINVAL;
    }

    android_dataspace_t dataspace = getDstDataspace(formatDesc.halFormat,
                                                    mAssignedDisplayInfo, dstImgInfo->dataspace);

    ExynosGraphicBufferMeta gmeta(dstHandle);

    bufFds[0] = gmeta.fd;
    bufFds[1] = gmeta.fd1;
    bufFds[2] = gmeta.fd2;
    if (getBufLength(dstHandle, MAX_HW2D_PLANES, bufLength, formatDesc.halFormat,
                     gmeta.stride, gmeta.vstride) != NO_ERROR) {
        MPP_LOGE("%s:: invalid bufferLength(%zu, %zu, %zu), format(%s)", __func__,
                 bufLength[0], bufLength[1], bufLength[2], dstImgInfo->format.name().c_str());
        return -EINVAL;
    }

    dstImgInfo->bufferType = getBufferType(dstHandle);
    if (dstImgInfo->bufferType == MPP_BUFFER_SECURE_DRM)
        attribute |= AcrylicCanvas::ATTR_PROTECTED;

    if (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) {
        int32_t xres = pixel_align(mAssignedDisplayInfo.xres, GET_M2M_DST_ALIGN(formatDesc.halFormat));
        int32_t yres = pixel_align(mAssignedDisplayInfo.yres, GET_M2M_DST_ALIGN(formatDesc.halFormat));
        mAcrylicHandle->setCanvasDimension(xres, yres);
    }

    /* setup dst */
    if (isComposition && (isAFBCCompressed(dstHandle)) &&
        mCurrentTargetCompressionInfoType == COMP_TYPE_AFBC)
        attribute |= AcrylicCanvas::ATTR_COMPRESSED;

    mFenceTracer.setFenceInfo(dstImgInfo->acrylicReleaseFenceFd,
                              mAssignedDisplayInfo.displayIdentifier, FENCE_TYPE_DST_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType), FENCE_TO);

    mAcrylicHandle->setCanvasImageType(formatDesc.halFormat, dataspace);

    if ((mLogicalType == MPP_LOGICAL_G2D_COMBO) &&
        (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
        (mAssignedDisplayInfo.displayIdentifier.type == HWC_DISPLAY_VIRTUAL) &&
        (mAssignedDisplayInfo.isWFDState == (int)LLWFD)) {
        mAcrylicHandle->setCanvasImageType(HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN, dataspace);
        dstImgInfo->acrylicReleaseFenceFd = mFenceTracer.fence_close(
            dstImgInfo->acrylicReleaseFenceFd, mAssignedDisplayInfo.displayIdentifier,
            FENCE_TYPE_DST_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
            "mpp::setupDst: dst acrylicReleaseFence");
        mAcrylicHandle->setCanvasBuffer(bufFds, bufLength, bufferNum,
                                        dstImgInfo->acrylicReleaseFenceFd, attribute);
        mAcrylicHandle->setCanvasOTF(attribute);
    } else {
        dstImgInfo->acrylicReleaseFenceFd =
            mFenceTracer.checkFenceDebug(mAssignedDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_DST_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType), dstImgInfo->acrylicReleaseFenceFd);
        mAcrylicHandle->setCanvasBuffer(bufFds, bufLength, bufferNum,
                                        dstImgInfo->acrylicReleaseFenceFd, attribute);
    }

    dstMetaInfo_t metaInfo = getDstMetaInfo(dataspace);
    if ((mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) && (mPhysicalType == MPP_G2D) &&
        (mAssignedDisplayInfo.displayIdentifier.type != HWC_DISPLAY_VIRTUAL)) {
        mAcrylicHandle->setTargetDisplayLuminance(metaInfo.minLuminance, metaInfo.maxLuminance);
    }

    MPP_LOGD(eDebugMPP, "destination configuration:");
    MPP_LOGD(eDebugMPP, "\tImageDimension[%d, %d], ImageType[%s, %d], target luminance[%d, %d]",
             gmeta.stride, gmeta.vstride, dstImgInfo->format.name().c_str(),
             dataspace, metaInfo.minLuminance, metaInfo.maxLuminance);
    MPP_LOGD(eDebugMPP, "\tImageBuffer handle: %p, fds[%d, %d, %d], bufLength[%zu, %zu, %zu], bufferNum: %d, releaseFence: %d, attribute: %d",
             dstHandle, bufFds[0], bufFds[1], bufFds[2], bufLength[0], bufLength[1], bufLength[2],
             bufferNum, dstImgInfo->acrylicReleaseFenceFd, attribute);

    dstImgInfo->acrylicReleaseFenceFd = -1;
    dstImgInfo->dataspace = dataspace;

    return ret;
}

int32_t ExynosMPP::doPostProcessingInternal() {
    ATRACE_CALL();
    int ret = NO_ERROR;
    size_t sourceNum = mAssignedSources.size();

    if (mAcrylicHandle == NULL) {
        MPP_LOGE("%s:: mAcrylicHandle is NULL", __func__);
        return -EINVAL;
    }
    if ((mPhysicalType != MPP_G2D) && (mPhysicalType != MPP_MSC)) {
        MPP_LOGE("%s:: invalid mPhysicalType(%d)", __func__, mPhysicalType);
        return -EINVAL;
    }
    if (mAssignedDisplayInfo.displayIdentifier.id == UINT32_MAX) {
        MPP_LOGE("assigned display is not set");
        return -EINVAL;
    }

    /* setup source layers */
    for (size_t i = 0; i < sourceNum; i++) {
        MPP_LOGD(eDebugMPP, "Setup [%zu] source: %p", i, mAssignedSources[i]);
        if ((ret = setupLayer(&mSrcImgs[i], mAssignedSources[i]->mSrcImg, mAssignedSources[i]->mMidImg)) != NO_ERROR) {
            MPP_LOGE("%s:: fail to setupLayer[%zu], ret %d",
                     __func__, i, ret);
            return ret;
        }
    }

    if (mPrevFrameInfo.srcNum > sourceNum) {
        MPP_LOGD(eDebugMPP, "prev sourceNum(%d), current sourceNum(%zu)",
                 mPrevFrameInfo.srcNum, sourceNum);
        for (size_t i = sourceNum; i < mPrevFrameInfo.srcNum; i++) {
            MPP_LOGD(eDebugMPP, "Remove mSrcImgs[%zu], %p", i, mSrcImgs[i].mppLayer);
            if (mSrcImgs[i].mppLayer != NULL) {
                delete mSrcImgs[i].mppLayer;
                mSrcImgs[i].mppLayer = NULL;
            }
        }
    }

    if (mAcrylicHandle->layerCount() != mAssignedSources.size()) {
        MPP_LOGE("Different layer number, acrylic layers(%d), assigned size(%zu)",
                 mAcrylicHandle->layerCount(), mAssignedSources.size());
        return -EINVAL;
    }

    setupDst(&mDstImgs[mCurrentDstBuf]);

    int usingFenceCnt = 1;
    bool acrylicReturn = true;

#ifndef DISABLE_FENCE
    if (mUseM2MSrcFence)
        usingFenceCnt = sourceNum + 1;  // Get and Use src + dst fence
    else
        usingFenceCnt = 1;  // Get and Use only dst fence
    int *outFences = new int[usingFenceCnt];
    int dstBufIdx = usingFenceCnt - 1;
#else
    usingFenceCnt = 0;  // Get and Use no fences
    int dstBufIdx = 0;
    int *outFences = NULL;
#endif

    funcReturnCallback retCallback([&]() {
        if (outFences != nullptr)
            delete[] outFences;
    });

    {
        ATRACE_CALL();
        acrylicReturn = mAcrylicHandle->execute(outFences, usingFenceCnt);
    }

    for (size_t i = 0; i < sourceNum; i++) {
        mSrcImgs[i].mppLayer->clearLayerData();
    }

    auto setAcrylFence = [&]() {
        for (auto &src : mSrcImgs) {
            src.acrylicReleaseFenceFd = -1;
        }
        mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd = -1;
    };

    if (acrylicReturn == false) {
        MPP_LOGE("%s:: fail to excute compositor", __func__);
        setAcrylFence();
        String8 dumpStr;
        dumpBufInfo(dumpStr);
        ALOGD("%s", dumpStr.c_str());
        return -EPERM;
    }

    // set fence informations from acryl
    mFenceTracer.setFenceInfo(outFences[dstBufIdx],
                              mAssignedDisplayInfo.displayIdentifier,
                              FENCE_TYPE_DST_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType), FENCE_FROM);

    if (usingFenceCnt > 1) {
        for (size_t i = 0; i < sourceNum; i++) {
            // TODO DPU release fence is tranferred to m2mMPP's source layer fence
            mFenceTracer.setFenceInfo(outFences[i],
                                      mAssignedDisplayInfo.displayIdentifier,
                                      FENCE_TYPE_SRC_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType), FENCE_FROM);
        }
    }

    if ((mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
        (mAssignedDisplayInfo.displayIdentifier.type == HWC_DISPLAY_VIRTUAL) &&
        (!mAssignedDisplayInfo.useDpu) &&
        (mAssignedDisplayInfo.isWFDState == (int)LLWFD) &&
        (usingFenceCnt != 0)) {  // Use no fences
        outFences[dstBufIdx] = mFenceTracer.fence_close(
            outFences[dstBufIdx], mAssignedDisplayInfo.displayIdentifier,
            FENCE_TYPE_SRC_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
            "mpp::doPostProcessingInternal:MPP dst out fence for WFD");  // Close dst buf's fence
    }

    if (usingFenceCnt == 0) {  // Use no fences
        setAcrylFence();
    } else {
        for (size_t i = 0; i < sourceNum; i++) {
            if (mUseM2MSrcFence)
                mSrcImgs[i].acrylicReleaseFenceFd =
                    mFenceTracer.checkFenceDebug(mAssignedDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType), outFences[i]);
            else
                mSrcImgs[i].acrylicReleaseFenceFd = -1;
        }

        if (mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd >= 0) {
            MPP_LOGE("mDstImgs[%d].acrylicAcquireFenceFd(%d) is not initialized",
                     mCurrentDstBuf,
                     mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd);
        }

        if (mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd >= 0)
            mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd =
                mFenceTracer.fence_close(
                    mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd,
                    mAssignedDisplayInfo.displayIdentifier,
                    FENCE_TYPE_DST_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
                    "mpp::doPostProcessingInternal: MPP dst acrylicAcquireFence");
        mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd =
            mFenceTracer.checkFenceDebug(mAssignedDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_DST_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType), outFences[dstBufIdx]);
    }

    dumpDstBuf();

    return ret;
}

void ExynosMPP::dumpDstBuf() {
    if (!exynosHWCControl.dumpMidBuf)
        return;

    MPP_LOGI("dump image");
    exynosHWCControl.dumpMidBuf = false;

    if ((mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd > 0) &&
        (sync_wait(mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd, 1000) < 0)) {
        MPP_LOGE("%s:: fence sync_wait error to dump image", __func__);
        return;
    }

    buffer_handle_t dstHandle = mDstImgs[mCurrentDstBuf].bufferHandle;
    ExynosGraphicBufferMeta gmeta(dstHandle);

    MPP_LOGI("dump image fw: %d, fh:%d, size: %d", gmeta.stride, gmeta.vstride, gmeta.size);
    FILE *fp = fopen(MPP_DUMP_PATH, "ab");
    if (fp == nullptr) {
        MPP_LOGE("open fail %s", strerror(errno));
        return;
    }

    void *temp = mmap(0, gmeta.size, PROT_READ | PROT_WRITE, MAP_SHARED, gmeta.fd, 0);
    if (temp == nullptr) {
        MPP_LOGE("mmap is NULL %s", strerror(errno));
        fclose(fp);
        return;
    }

    MPP_LOGI("write...%p", temp);
    int write_size = fwrite(temp, gmeta.size, 1, fp);
    if (write_size < 0) {
        MPP_LOGI("write error: %s", strerror(errno));
    } else {
        MPP_LOGI("write size: %d", write_size);
    }
    munmap(temp, gmeta.size);
    fclose(fp);
}

bool ExynosMPP::canSkipProcessing() {
    if ((mAssignedDisplayInfo.displayIdentifier.id == UINT32_MAX) ||
        (mAssignedSources.size() == 0))
        return true;
    ExynosMPPSource *source = mAssignedSources[0];
    exynos_image dst = source->mMidImg;
    if (mMaxSrcLayerNum > 1) {
        /*
         * Initialize fields that are used by needDstBufRealloc()
         * This should be updated if the fileds that are checked by
         * needDstBufRealloc() is changed
         */
        dst.reset();
        dst.exynosFormat = defaultMppDstFormat;
        dst.fullWidth = mAssignedDisplayInfo.xres;
        dst.fullHeight = mAssignedDisplayInfo.yres;
        dst.dataSpace = colorModeToDataspace(mAssignedDisplayInfo.colorMode);
    }
    return ((needDstBufRealloc(dst, mCurrentDstBuf) == false) & canUsePrevFrame(source->mSrcImg));
}

/**
 * @param src
 * @param dst
 * @return int32_t releaseFenceFd of src buffer
 */
int32_t ExynosMPP::doPostProcessing(struct exynos_image &src, struct exynos_image &dst) {
    ATRACE_CALL();
    MPP_LOGD(eDebugMPP, "total assigned sources (%zu)++++++++", mAssignedSources.size());

    auto save_frame_info = [=, this]() {
        /* Save current frame information for next frame*/
        mPrevAssignedDisplayType = mAssignedDisplayInfo.displayIdentifier.type;
        mPrevFrameInfo.srcNum = (uint32_t)mAssignedSources.size();
        for (uint32_t i = 0; i < mPrevFrameInfo.srcNum; i++) {
            mPrevFrameInfo.srcInfo[i] = mAssignedSources[i]->mSrcImg;
            mPrevFrameInfo.dstInfo[i] = mAssignedSources[i]->mMidImg;
        }

        MPP_LOGD(eDebugMPP, "mPrevAssignedState: %d, mPrevAssignedDisplayType: %d--------------",
                 mAssignedState, mAssignedDisplayInfo.displayIdentifier.type);
    };

    int ret = NO_ERROR;
    bool realloc = false;
    if (mAssignedSources.size() == 0) {
        MPP_LOGE("Assigned source size(%zu) is not valid",
                 mAssignedSources.size());
        save_frame_info();
        return -EINVAL;
    }

    // Check whether destination buffer allocation is required
    if (mAllocOutBufFlag) {
        if ((realloc = needDstBufRealloc(dst, mCurrentDstBuf)) == true) {
            //  allocate mDstImgs[mCurrentDstBuf]
            bool isComposition = (mMaxSrcLayerNum > 1);
            if (isComposition)
                dst.exynosFormat = defaultMppDstFormat;

            uint32_t allocFormat = dst.exynosFormat.halFormat();
            if (mFreeOutBufFlag == false)
                allocFormat = DEFAULT_MPP_DST_FORMAT;

            if ((allocFormat == HAL_PIXEL_FORMAT_RGBA_1010102) ||
                (allocFormat == HAL_PIXEL_FORMAT_YCBCR_P010))
                allocFormat = DEFAULT_MPP_DST_FORMAT;

            uint32_t bufAlign = GET_M2M_DST_ALIGN(allocFormat);
            ret = allocOutBuf(pixel_align(mAssignedDisplayInfo.xres, bufAlign),
                              pixel_align(mAssignedDisplayInfo.yres, bufAlign),
                              allocFormat, dst.usageFlags, mCurrentDstBuf);
        }
        if (ret < 0) {
            MPP_LOGE("%s:: fail to allocate dst buffer[%d]", __func__, mCurrentDstBuf);
            save_frame_info();
            return ret;
        }
        if (mDstImgs[mCurrentDstBuf].format != dst.exynosFormat) {
            MPP_LOGD(eDebugMPP, "dst format is changed (%s -> %s)",
                     mDstImgs[mCurrentDstBuf].format.name().c_str(), dst.exynosFormat.name().c_str());
            mDstImgs[mCurrentDstBuf].format = dst.exynosFormat;
        }
    }

    if ((realloc == false) && canUsePrevFrame(src)) {
        mCurrentDstBuf = (mCurrentDstBuf + NUM_MPP_DST_BUFS(mLogicalType) - 1) % NUM_MPP_DST_BUFS(mLogicalType);
        MPP_LOGD(eDebugMPP, "Reuse previous frame, dstImg[%d]", mCurrentDstBuf);
        for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
            mAssignedSources[i]->mSrcImg.acquireFenceFd =
                mFenceTracer.fence_close(mAssignedSources[i]->mSrcImg.acquireFenceFd,
                                         mAssignedDisplayInfo.displayIdentifier,
                                         FENCE_TYPE_SRC_ACQUIRE, mFenceTracer.getM2MIPFenceType(mPhysicalType),
                                         "mpp::doPostProcessing: src acquireFence in reuse case");
        }
        mWasUsedPrevFrame = true;
        save_frame_info();
        return 0;
    }

    /* G2D or sclaer case */
    if ((ret = doPostProcessingInternal()) < 0) {
        MPP_LOGE("%s:: fail to post processing, ret %d",
                 __func__, ret);
        save_frame_info();
        return ret;
    }

    save_frame_info();
    return 0;
}

/*
 * This function should be called after doPostProcessing()
 * because doPostProcessing() sets
 * mSrcImgs[].mppImg.releaseFenceFd
 */
int32_t ExynosMPP::getSrcReleaseFence(uint32_t srcIndex) {
    if (srcIndex >= NUM_MPP_SRC_BUFS)
        return -EINVAL;

    return mSrcImgs[srcIndex].acrylicReleaseFenceFd;

    return -EINVAL;
}

int32_t ExynosMPP::resetSrcReleaseFence() {
    for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
        mSrcImgs[i].acrylicReleaseFenceFd = -1;
    }
    return NO_ERROR;
}

int32_t ExynosMPP::getDstImageInfo(exynos_image *img) {
    if ((mCurrentDstBuf < 0) || (mCurrentDstBuf >= NUM_MPP_DST_BUFS(mLogicalType)) ||
        (mAssignedDisplayInfo.displayIdentifier.id == UINT32_MAX)) {
        MPP_LOGE("mCurrentDstBuf(%d), mAssignedDisplay(0x%8x)",
                 mCurrentDstBuf, mAssignedDisplayInfo.displayIdentifier.id);
        return -EINVAL;
    }

    img->reset();

    if (mDstImgs[mCurrentDstBuf].bufferHandle == NULL) {
        img->acquireFenceFd = -1;
        img->releaseFenceFd = -1;
        return -EFAULT;
    } else {
        img->bufferHandle = mDstImgs[mCurrentDstBuf].bufferHandle;
        img->fullWidth = pixel_align(mAssignedDisplayInfo.xres, GET_M2M_DST_ALIGN(mDstImgs[mCurrentDstBuf].format.halFormat()));
        img->fullHeight = pixel_align(mAssignedDisplayInfo.yres, GET_M2M_DST_ALIGN(mDstImgs[mCurrentDstBuf].format.halFormat()));
        if (mMaxSrcLayerNum > 1) {
            if (mAssignedSources.size() == 1) {
                img->x = mAssignedSources[0]->mDstImg.x;
                img->y = mAssignedSources[0]->mDstImg.y;
                img->w = mAssignedSources[0]->mDstImg.w;
                img->h = mAssignedSources[0]->mDstImg.h;
            } else {
                img->x = 0;
                img->y = 0;
                img->w = mAssignedDisplayInfo.xres;
                img->h = mAssignedDisplayInfo.yres;
            }
        } else {
            img->x = mAssignedSources[0]->mMidImg.x;
            img->y = mAssignedSources[0]->mMidImg.y;
            img->w = mAssignedSources[0]->mMidImg.w;
            img->h = mAssignedSources[0]->mMidImg.h;
            img->needColorTransform =
                mAssignedSources[0]->mMidImg.needColorTransform;
        }

        img->exynosFormat = mDstImgs[mCurrentDstBuf].format;
        img->acquireFenceFd = mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd;
        img->releaseFenceFd = mDstImgs[mCurrentDstBuf].acrylicReleaseFenceFd;
        img->dataSpace = mDstImgs[mCurrentDstBuf].dataspace;
    }
    return NO_ERROR;
}

/*
 * This function should be called after getDstReleaseFence()
 * by ExynosDisplay
 */
int32_t ExynosMPP::setDstReleaseFence(int releaseFence, DisplayInfo &display) {
    int dstBufIndex = 0;

    dstBufIndex = mPrivDstBuf;

    if (mPrivDstBuf == mCurrentDstBuf)
        MPP_LOGD(eDebugMPP,
                 "M2MMPP : same buffer was reused idx %d, %d", mPrivDstBuf, mCurrentDstBuf);

    if (mAssignedDisplayInfo.displayIdentifier.id == UINT32_MAX)
        mAssignedDisplayInfo = display;

    if (dstBufIndex < 0 || dstBufIndex >= NUM_MPP_DST_BUFS(mLogicalType)) {
        releaseFence = mFenceTracer.fence_close(releaseFence,
                                                mAssignedDisplayInfo.displayIdentifier,
                                                FENCE_TYPE_DST_RELEASE, FENCE_IP_ALL,
                                                "mpp::setDstReleaseFence: releaseFence in error case");
        mPrivDstBuf = mCurrentDstBuf;
        return -EINVAL;
    }

    if (mDstImgs[dstBufIndex].acrylicReleaseFenceFd >= 0) {
        mFenceTracer.fence_close(mDstImgs[dstBufIndex].acrylicReleaseFenceFd,
                                 mAssignedDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_DST_RELEASE, FENCE_IP_ALL,
                                 "mpp::setDstReleaseFence: prev dst acrylicReleaseFence");
    }

    mDstImgs[dstBufIndex].acrylicReleaseFenceFd =
        mFenceTracer.checkFenceDebug(mAssignedDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_DST_RELEASE, mFenceTracer.getM2MIPFenceType(mPhysicalType), releaseFence);

    mPrivDstBuf = mCurrentDstBuf;

    return NO_ERROR;
}

int32_t ExynosMPP::resetDstAcquireFence() {
    if (mCurrentDstBuf < 0 || mCurrentDstBuf >= NUM_MPP_DST_BUFS(mLogicalType))
        return -EINVAL;

    mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd = -1;

    return NO_ERROR;
}

int32_t ExynosMPP::requestHWStateChange(uint32_t state) {
    MPP_LOGD(eDebugMPP | eDebugBuf, "state: %d", state);
    /* Set HW state to running */
    if (mHWState == state) {
        if ((mPhysicalType == MPP_G2D) && (state == MPP_HW_STATE_IDLE) && (mHWBusyFlag == false)) {
            int ret = NO_ERROR;
            if (mAcrylicHandle == NULL) {
                MPP_LOGE("ArcicHandle is NULL");
                return NO_ERROR;
            } else {
                if ((ret = prioritize(-1)) != NO_ERROR)
                    MPP_LOGI("prioritize (%d) will be applied on next work", ret);
            }
        }
        return NO_ERROR;
    }

    if (state == MPP_HW_STATE_RUNNING)
        mHWState = MPP_HW_STATE_RUNNING;
    else if (state == MPP_HW_STATE_IDLE) {
        if (mLastStateFenceFd >= 0)
            mResourceManageThread.addStateFence(mLastStateFenceFd);
        else
            mHWState = MPP_HW_STATE_IDLE;
        mLastStateFenceFd = -1;

        if ((mPhysicalType == MPP_G2D) && (mHWBusyFlag == false)) {
            int ret = NO_ERROR;
            if ((ret = prioritize(-1)) != NO_ERROR)
                MPP_LOGI("prioritize (%d) is not applied on next work", ret);
        }

        /* Free all of output buffers */
        if (mMPPType == MPP_TYPE_M2M) {
            for (uint32_t i = 0; i < NUM_MPP_DST_BUFS(mLogicalType); i++) {
                exynos_mpp_img_info freeDstBuf = mDstImgs[i];
                mDstImgs[i].reset();
                mDstImgs[i].acrylicAcquireFenceFd = freeDstBuf.acrylicAcquireFenceFd;
                mDstImgs[i].acrylicReleaseFenceFd = freeDstBuf.acrylicReleaseFenceFd;
                freeDstBuf.acrylicAcquireFenceFd = -1;
                freeDstBuf.acrylicReleaseFenceFd = -1;

                if (mFreeOutBufFlag == true) {
                    MPP_LOGD(eDebugMPP | eDebugBuf, "free outbuf[%d] %p",
                             i, freeDstBuf.bufferHandle);
                    if (freeDstBuf.bufferHandle != NULL && mAllocOutBufFlag) {
                        freeOutBuf(freeDstBuf);
                    }
                } else {
                    mDstImgs[i].bufferHandle = freeDstBuf.bufferHandle;
                    mDstImgs[i].bufferType = freeDstBuf.bufferType;
                }
            }
        }

        for (uint32_t i = 0; i < NUM_MPP_SRC_BUFS; i++) {
            if (mSrcImgs[i].mppLayer != NULL) {
                delete mSrcImgs[i].mppLayer;
                mSrcImgs[i].mppLayer = NULL;
            }
        }
        mPrevFrameInfo.reset();
    }

    return NO_ERROR;
}

int32_t ExynosMPP::setHWStateFence(int32_t fence) {
    mLastStateFenceFd = fence;
    return NO_ERROR;
}

int64_t ExynosMPP::checkDstSize(exynos_image &dst) {
    uint32_t maxDstWidth = getDstMaxWidth(dst);
    uint32_t maxDstHeight = getDstMaxHeight(dst);
    uint32_t minDstWidth = getDstMinWidth(dst);
    uint32_t minDstHeight = getDstMinHeight(dst);
    uint32_t dstWidthAlign = getDstWidthAlign(dst);
    uint32_t dstHeightAlign = getDstHeightAlign(dst);
    uint32_t dstXOffsetAlign = getDstXOffsetAlign(dst);
    uint32_t dstYOffsetAlign = getDstYOffsetAlign(dst);

    if (dst.w > maxDstWidth)
        return -eMPPExeedMaxDstWidth;
    if (dst.h > maxDstHeight)
        return -eMPPExeedMaxDstHeight;
    if (dst.w < minDstWidth)
        return -eMPPExeedMinDstWidth;
    if (dst.h < minDstHeight)
        return -eMPPExeedMinDstHeight;
    if ((dst.w % dstWidthAlign != 0) || (dst.h % dstHeightAlign != 0))
        return -eMPPNotAlignedDstSize;
    if ((dst.x % dstXOffsetAlign != 0) || (dst.y % dstYOffsetAlign != 0))
        return -eMPPNotAlignedOffset;

    return NO_ERROR;
}

int64_t ExynosMPP::checkUnResizableSrcSize(exynos_image &src) {
    uint32_t maxSrcWidth = getSrcMaxWidth(src);
    uint32_t minSrcWidth = getSrcMinWidth(src);
    uint32_t minSrcHeight = getSrcMinHeight(src);
    uint32_t srcWidthAlign = getSrcWidthAlign(src);
    uint32_t maxSrcCropSize = getSrcMaxCropSize(src);
    uint32_t minSrcCropWidth = getSrcMinCropWidth(src);
    uint32_t minSrcCropHeight = getSrcMinCropHeight(src);

    if (src.fullWidth < minSrcWidth)
        return -eMPPExeedMinSrcWidth;
    if (src.fullHeight < minSrcHeight)
        return -eMPPExeedMinSrcHeight;
    if (src.fullWidth > maxSrcWidth)
        return -eMPPExceedHStrideMaximum;
    if (src.w < minSrcCropWidth)
        return -eMPPExeedSrcWCropMin;
    if (src.h < minSrcCropHeight)
        return -eMPPExeedSrcHCropMin;
    if ((src.w * src.h) > maxSrcCropSize)
        return -eMPPExeedSrcCropMax;
    if (src.fullWidth % srcWidthAlign != 0)
        return -eMPPNotAlignedHStride;

    return NO_ERROR;
}

int64_t ExynosMPP::checkResizableSrcSize(exynos_image &src) {
    uint32_t maxSrcHeight = getSrcMaxHeight(src);
    uint32_t srcHeightAlign = getSrcHeightAlign(src);
    uint32_t maxSrcCropWidth = getSrcMaxCropWidth(src);
    uint32_t maxSrcCropHeight = getSrcMaxCropHeight(src);
    uint32_t srcCropWidthAlign = getSrcCropWidthAlign(src);
    uint32_t srcCropHeightAlign = getSrcCropHeightAlign(src);
    uint32_t srcXOffsetAlign = getSrcXOffsetAlign(src);
    uint32_t srcYOffsetAlign = getSrcYOffsetAlign(src);

    if (src.fullHeight > maxSrcHeight)
        return -eMPPExceedVStrideMaximum;
    if (src.fullHeight % srcHeightAlign != 0)
        return -eMPPNotAlignedVStride;
    if (src.w > maxSrcCropWidth)
        return -eMPPExeedSrcWCropMax;
    if (src.h > maxSrcCropHeight)
        return -eMPPExeedSrcHCropMax;
    if ((src.w % srcCropWidthAlign != 0) || (src.h % srcCropHeightAlign != 0))
        return -eMPPNotAlignedCrop;
    if ((src.x % srcXOffsetAlign != 0) || (src.y % srcYOffsetAlign != 0))
        return -eMPPNotAlignedOffset;

    return NO_ERROR;
}

int64_t ExynosMPP::checkScaleRatio(DisplayInfo &display, exynos_image &src,
                                   exynos_image &dst) {
    exynos_image rot_dst = dst;
    bool isPerpendicular = !!(src.transform & HAL_TRANSFORM_ROT_90);
    if (isPerpendicular) {
        rot_dst.w = dst.h;
        rot_dst.h = dst.w;
    }
    uint32_t maxDownscale = getMaxDownscale(display, src, dst);
    uint32_t maxUpscale = getMaxUpscale(src, dst);

    if (src.w > rot_dst.w * maxDownscale)
        return -eMPPExeedMaxDownScale;
    if (rot_dst.w > src.w * maxUpscale)
        return -eMPPExeedMaxUpScale;
    if (src.h > rot_dst.h * maxDownscale)
        return -eMPPExeedMaxDownScale;
    if (rot_dst.h > src.h * maxUpscale)
        return -eMPPExeedMaxUpScale;

    return NO_ERROR;
}

int64_t ExynosMPP::isSupported(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst) {
    int32_t ret = NO_ERROR;

    if ((ret = checkDstSize(dst)) < 0)
        return ret;

    /* for virtual 8K MPP */
    if (isSharedMPPUsed())
        return -eMPPConflictSharedMPP;

    if (src.isDimLayer())  // Dim layer
    {
        return isDimLayerSupported();
    }

    if (!isSupportedCapability(display, src))
        return -eMPPSaveCapability;
    if (!isSrcFormatSupported(src))
        return -eMPPUnsupportedFormat;
    if (!isDstFormatSupported(dst))
        return -eMPPUnsupportedFormat;
    if (!isDataspaceSupportedByMPP(src, dst))
        return -eMPPUnsupportedCSC;
    if (!isSupportedHDR10Plus(src, dst))
        return -eMPPUnsupportedDynamicMeta;
    if (!isSupportedBlend(src))
        return -eMPPUnsupportedBlending;
    if (!isSupportedTransform(src))
        return -eMPPUnsupportedRotation;
    if ((ret = checkUnResizableSrcSize(src)) < 0)
        return ret;
    if (!isSupportedDRM(src))
        return -eMPPUnsupportedDRM;
    if ((getDrmMode(src.usageFlags) == NO_DRM) &&
        ((ret = checkResizableSrcSize(src)) < 0))
        return ret;
    if ((ret = checkScaleRatio(display, src, dst)) < 0)
        return ret;
    if (!isSupportedHStrideCrop(src))
        return -eMPPStrideCrop;

    if (!isSupportedCompression(src))
        return -eMPPUnsupportedCompression;

    if (!isSupportLayerColorTransform(src, dst))
        return -eMPPUnsupportedColorTransform;

    return NO_ERROR;
}

int32_t ExynosMPP::resetMPP() {
    mAssignedState = MPP_ASSIGN_STATE_FREE;
    mAssignedDisplayInfo.reset();
    mAssignedSources.clear();
    resetUsedCapacity();
    mHWBusyFlag = false;

    return NO_ERROR;
}

int32_t ExynosMPP::resetAssignedState() {
    for (int i = (int)mAssignedSources.size(); i-- > 0;) {
        ExynosMPPSource *mppSource = mAssignedSources[i];
        if (mppSource->mOtfMPP == this) {
            mppSource->mOtfMPP = NULL;
        }
        if (mppSource->mM2mMPP == this) {
            mppSource->mM2mMPP = NULL;
        }
        mAssignedSources.removeItemsAt(i);
    }

    /* Keep status if mAssignedState is MPP_ASSIGN_STATE_RESERVED */
    if ((mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) &&
        (mAssignedSources.size() == 0)) {
        mAssignedState &= ~MPP_ASSIGN_STATE_ASSIGNED;
        mAssignedDisplayInfo.reset();
    }

    /* All mpp source are removed, reset capacity information */
    resetUsedCapacity();

    return NO_ERROR;
}

int32_t ExynosMPP::resetAssignedState(ExynosMPPSource *mppSource) {
    bool needUpdateCapacity = false;
    for (int i = (int)mAssignedSources.size(); i-- > 0;) {
        ExynosMPPSource *source = mAssignedSources[i];
        if (source == mppSource) {
            if (mppSource->mM2mMPP == this) {
                mppSource->mM2mMPP = NULL;
            }
            /* Update information for used capacity */
            /* This should be called before mAssignedSources.removeItemsAt(mppSource) */
            needUpdateCapacity = removeCapacity(mppSource);

            mAssignedSources.removeItemsAt(i);
            if (needUpdateCapacity)
                updateUsedCapacity();

            break;
        }
    }

    /* Keep status if mAssignedState is MPP_ASSIGN_STATE_RESERVED */
    if ((mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) &&
        (mAssignedSources.size() == 0)) {
        mAssignedState &= ~MPP_ASSIGN_STATE_ASSIGNED;
        mAssignedDisplayInfo.reset();
    }

    return NO_ERROR;
}

int32_t ExynosMPP::reserveMPP(DisplayInfo &display) {
    mAssignedState |= MPP_ASSIGN_STATE_RESERVED;
    mReservedDisplayInfo = display;

    return NO_ERROR;
}

int32_t ExynosMPP::assignMPP(DisplayInfo &display, ExynosMPPSource *mppSource) {
    mAssignedState |= MPP_ASSIGN_STATE_ASSIGNED;

    if (mMPPType == MPP_TYPE_OTF)
        mppSource->mOtfMPP = this;
    else if (mMPPType == MPP_TYPE_M2M)
        mppSource->mM2mMPP = this;
    else {
        MPP_LOGE("%s:: Invalid mppType(%d)", __func__, mMPPType);
        return -EINVAL;
    }

    mAssignedDisplayInfo = display;

    /* Update information for used capacity */
    /* This should be called before mAssignedSources.add(mppSource) */
    bool needUpdateCapacity = addCapacity(mppSource);

    mAssignedSources.add(mppSource);

    MPP_LOGD(eDebugCapacity | eDebugMPP, "\tassigned to source(%p) type(%d), mAssignedSources(%zu)",
             mppSource, mppSource->mSourceType,
             mAssignedSources.size());

    if (needUpdateCapacity)
        updateUsedCapacity();

    if (mMaxSrcLayerNum > 1) {
        std::sort(mAssignedSources.begin(), mAssignedSources.end(), exynosMPPSourceComp);
    }

    return NO_ERROR;
}

uint32_t ExynosMPP::getSrcMaxBlendingNum(struct exynos_image __unused &src, struct exynos_image __unused &dst) {
    return mMaxSrcLayerNum;
}

uint32_t ExynosMPP::getAssignedSourceNum() {
    return mAssignedSources.size();
}

bool ExynosMPP::needPreAllocation(uint32_t displayMode) {
    bool ret = false;

    if ((mLogicalType == MPP_LOGICAL_G2D_RGB) &&
        (mPreAssignDisplayList[displayMode] == HWC_DISPLAY_PRIMARY_BIT))
        ret = true;

    return ret;
}

bool ExynosMPP::isAssignableState(DisplayInfo &display,
                                  struct exynos_image &src, struct exynos_image &dst) {
    if (mDisableByUserScenario) {
        MPP_LOGD(eDebugMPP, "\tmDisableByUserScenario(0x%8x)", mDisableByUserScenario);
        return false;
    }

    bool isAssignable = false;

    if (mAssignedState == MPP_ASSIGN_STATE_FREE) {
        if (mHWState == MPP_HW_STATE_IDLE)
            isAssignable = true;
        else {
            if ((mPrevAssignedDisplayType < 0) ||
                ((uint32_t)mPrevAssignedDisplayType == display.displayIdentifier.type))
                isAssignable = true;
            else
                isAssignable = false;
        }
    }

    if ((mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) && (mAssignedState & MPP_ASSIGN_STATE_RESERVED)) {
        if (mReservedDisplayInfo.displayIdentifier.id == display.displayIdentifier.id) {
            if (mAssignedSources.size() < getSrcMaxBlendingNum(src, dst))
                isAssignable = true;
            else
                isAssignable = false;
        } else {
            isAssignable = false;
        }
    } else if ((mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) && !(mAssignedState & MPP_ASSIGN_STATE_RESERVED)) {
        if (mAssignedSources.size() < getSrcMaxBlendingNum(src, dst))
            isAssignable = true;
        else
            isAssignable = false;
    } else if (mAssignedState & MPP_ASSIGN_STATE_RESERVED) {
        if (mReservedDisplayInfo.displayIdentifier.id == display.displayIdentifier.id)
            isAssignable = true;
        else
            isAssignable = false;
    }

    MPP_LOGD(eDebugMPP, "\tisAssignableState(%d), mAssignedState(%d), mHWState(%d), "
                        "mPrevAssignedDisplayType(%d), mReservedDisplay(%d), assigned size(%zu), getSrcMaxBlendingNum(%d)",
             isAssignable, mAssignedState, mHWState, mPrevAssignedDisplayType,
             mReservedDisplayInfo.displayIdentifier.id, mAssignedSources.size(),
             getSrcMaxBlendingNum(src, dst));
    return isAssignable;
}
bool ExynosMPP::isAssignable(DisplayInfo &display, struct exynos_image &src,
                             struct exynos_image &dst, float totalUsedCapacity) {
    bool isAssignable = isAssignableState(display, src, dst);
    return (isAssignable && hasEnoughCapa(display, src, dst, totalUsedCapacity));
}

bool ExynosMPP::hasEnoughCapa(DisplayInfo &display, struct exynos_image &src,
                              struct exynos_image &dst, float totalUsedCapacity) {
    if (mCapacity == -1)
        return true;

    MPP_LOGD(eDebugCapacity | eDebugMPP, "totalUsedCapacity(%f), mUsedCapacity(%f), preAssignedCapacity(%f)",
             totalUsedCapacity, mUsedCapacity, mPreAssignedCapacity);

    /* mUsedCapacity should be re-calculated including src, dst passed as parameters*/
    totalUsedCapacity -= mUsedCapacity;

    if ((mReservedDisplayInfo.displayIdentifier.id == display.displayIdentifier.id) &&
        (mPreAssignedCapacity > (float)0.0))
        totalUsedCapacity -= mPreAssignedCapacity;

    float requiredCapacity = getRequiredCapacity(display, src, dst);

    MPP_LOGD(eDebugCapacity | eDebugMPP, "mCapacity(%f), usedCapacity(%f), RequiredCapacity(%f)",
             mCapacity, totalUsedCapacity, requiredCapacity);

    if (mCapacity >= (totalUsedCapacity + requiredCapacity)) {
        return true;
    } else if (isCapacityExceptionCondition(totalUsedCapacity, requiredCapacity, src)) {
        return true;
    } else {
        return false;
    }
}

bool ExynosMPP::isCapacityExceptionCondition(float totalUsedCapacity, float requiredCapacity, struct exynos_image &src) {
    if ((hasHdrInfo(src) &&
         (totalUsedCapacity == 0) &&
         (requiredCapacity < (mCapacity * MPP_HDR_MARGIN)))) {
        return true;
    } else {
        return false;
    }
}

void ExynosMPP::getPPCIndex(const struct exynos_image &src,
                            const struct exynos_image &dst,
                            uint32_t &formatIndex, uint32_t &rotIndex, uint32_t &scaleIndex,
                            const struct exynos_image &criteria) {
    formatIndex = 0;
    rotIndex = 0;
    scaleIndex = 0;

    /* Compare SBWC and 10bitYUV420 first! because can be overlapped with other format */
    if (criteria.exynosFormat.isSBWC() && hasPPC(mPhysicalType, PPC_FORMAT_SBWC, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_SBWC;
    else if (criteria.exynosFormat.isP010() && hasPPC(mPhysicalType, PPC_FORMAT_P010, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_P010;
    else if (criteria.exynosFormat.isYUV8_2() && hasPPC(mPhysicalType, PPC_FORMAT_YUV8_2, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_YUV8_2;
    else if (criteria.exynosFormat.isYUV420() && hasPPC(mPhysicalType, PPC_FORMAT_YUV420, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_YUV420;
    else if (criteria.exynosFormat.isYUV422() && hasPPC(mPhysicalType, PPC_FORMAT_YUV422, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_YUV422;
    else if ((src.compressionInfo.type == COMP_TYPE_AFBC) && hasPPC(mPhysicalType, PPC_FORMAT_AFBC, PPC_ROT_NO))
        formatIndex = PPC_FORMAT_AFBC;
    else
        formatIndex = PPC_FORMAT_RGB32;

    if (((criteria.transform & HAL_TRANSFORM_ROT_90) != 0) ||
        (mRotatedSrcCropBW > 0))
        rotIndex = PPC_ROT;
    else
        rotIndex = PPC_ROT_NO;

    uint32_t srcResolution = src.w * src.h;
    uint32_t dstResolution = dst.w * dst.h;

    if (mPhysicalType == MPP_G2D) {
        if (srcResolution == dstResolution) {
            scaleIndex = PPC_SCALE_NO;
        } else if (dstResolution > srcResolution) {
            /* scale up case */
            if (dstResolution >= (srcResolution * 4))
                scaleIndex = PPC_SCALE_UP_4_;
            else
                scaleIndex = PPC_SCALE_UP_1_4;
        } else {
            /* scale down case */
            if ((dstResolution * 16) <= srcResolution)
                scaleIndex = PPC_SCALE_DOWN_16_;
            else if (((dstResolution * 9) <= srcResolution) &&
                     (srcResolution < (dstResolution * 16)))
                scaleIndex = PPC_SCALE_DOWN_9_16;
            else if (((dstResolution * 4) <= srcResolution) &&
                     (srcResolution < (dstResolution * 9)))
                scaleIndex = PPC_SCALE_DOWN_4_9;
            else
                scaleIndex = PPC_SCALE_DOWN_1_4;
        }
    } else
        scaleIndex = 0; /* MSC doesn't refer scale Index */
}

float ExynosMPP::getPPC(const struct exynos_image &src,
                        const struct exynos_image &dst, const struct exynos_image &criteria,
                        const struct exynos_image *assignCheckSrc,
                        const struct exynos_image *assignCheckDst) {
    float PPC = 0;
    uint32_t formatIndex = 0;
    uint32_t rotIndex = 0;
    uint32_t scaleIndex = 0;

    if ((mPhysicalType == MPP_G2D) &&
        (src.layerFlags & EXYNOS_HWC_DIM_LAYER))
        return G2D_BASE_PPC_COLORFILL;

    getPPCIndex(src, dst, formatIndex, rotIndex, scaleIndex, criteria);

    if ((rotIndex == PPC_ROT_NO) && (assignCheckSrc != NULL) &&
        ((assignCheckSrc->transform & HAL_TRANSFORM_ROT_90) != 0)) {
        rotIndex = PPC_ROT;
    }

    if (mPhysicalType == MPP_G2D || mPhysicalType == MPP_MSC) {
        if (hasPPC(mPhysicalType, formatIndex, rotIndex)) {
            auto node = ppc_table_map.find(PPC_IDX(mPhysicalType, formatIndex, rotIndex));
            if (node != ppc_table_map.end())
                PPC = node->second.ppcList[scaleIndex];
            else
                PPC = 0.0;
        }
    }

    if (PPC == 0) {
        MPP_LOGE("%s:: mPhysicalType(%d), formatIndex(%d), rotIndex(%d), scaleIndex(%d), PPC(%f) is not valid",
                 __func__, mPhysicalType, formatIndex, rotIndex, scaleIndex, PPC);
        PPC = 0.000001; /* It means can't use mPhysicalType H/W  */
    }

    MPP_LOGD(eDebugCapacity, "srcW(%d), srcH(%d), dstW(%d), dstH(%d), rot(%d)"
                             "formatIndex(%d), rotIndex(%d), scaleIndex(%d), PPC(%f)",
             src.w, src.h, dst.w, dst.h, src.transform,
             formatIndex, rotIndex, scaleIndex, PPC);
    return PPC;
}

float ExynosMPP::getAssignedCapacity() {
    float capacity = 0;
    float baseCycles = 0;
    uint32_t rotIndex = 0;

    if (mPhysicalType != MPP_G2D)
        return 0;

    /*
     * Client target is assigned to m2mMPP
     * even if capacity is not enough
     */
    if ((mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
        (mAssignedDisplayInfo.displayIdentifier.type == HWC_DISPLAY_VIRTUAL))
        return 0;

    for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
        if ((mAssignedSources[i]->mSrcImg.transform & HAL_TRANSFORM_ROT_90) != 0)
            rotIndex = PPC_ROT;
    }

    MPP_LOGD(eDebugCapacity, "Check all of assigned layers cycles");
    /* PPC of layers that were added before should be changed */
    /* Check cycles of all assigned layers again */
    if ((mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) && (mMaxSrcLayerNum > 1)) {
        baseCycles += ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL);
        MPP_LOGD(eDebugCapacity, "colorfill cycles: %f, total cycles: %f",
                 ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL), baseCycles);
    }

    for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
        float srcCycles = 0;
        uint32_t srcResolution = mAssignedSources[i]->mSrcImg.w * mAssignedSources[i]->mSrcImg.h;
        uint32_t dstResolution = mAssignedSources[i]->mMidImg.w * mAssignedSources[i]->mMidImg.h;
        uint32_t maxResolution = max(srcResolution, dstResolution);
        float PPC = 0;

        if (mAssignedSources[i]->mSrcImg.layerFlags & EXYNOS_HWC_DIM_LAYER) {
            PPC = G2D_BASE_PPC_COLORFILL;
        } else {
            PPC = getPPC(mAssignedSources[i]->mSrcImg, mAssignedSources[i]->mMidImg, mAssignedSources[i]->mSrcImg,
                         &mAssignedSources[i]->mSrcImg, &mAssignedSources[i]->mMidImg);
        }
        srcCycles = maxResolution / PPC;

        /* Hdr and drm layer is exception */
        if ((hasHdrInfo(mAssignedSources[i]->mSrcImg) ||
             (getDrmMode(mAssignedSources[i]->mSrcImg.usageFlags) != NO_DRM))) {
            MPP_LOGD(eDebugCapacity, "Src[%d] is skipped(drm or hdr), cycles: %f, PPC: %f, srcResolution: %d, dstResolution: %d, rot(%d)",
                     i, srcCycles, PPC, srcResolution, dstResolution, mAssignedSources[i]->mSrcImg.transform);
            continue;
        }

        baseCycles += srcCycles;

        MPP_LOGD(eDebugCapacity, "Src[%d] cycles: %f, total cycles: %f, PPC: %f, srcResolution: %d, dstResolution: %d, rot(%d)",
                 i, srcCycles, baseCycles, PPC, srcResolution, dstResolution, mAssignedSources[i]->mSrcImg.transform);
    }

    capacity = baseCycles / getMPPClock();

    return capacity;
}

float ExynosMPP::getRequiredCapacity(DisplayInfo &display,
                                     struct exynos_image &src,
                                     struct exynos_image &dst) {
    float capacity = 0;
    float cycles = 0;
    if (mPhysicalType == MPP_G2D) {
        /* Initialize value with the cycles that were already assigned */
        float baseCycles = mUsedBaseCycles;
        float srcCycles = 0;
        uint32_t srcResolution = src.w * src.h;
        uint32_t dstResolution = dst.w * dst.h;
        uint32_t maxResolution = max(srcResolution, dstResolution);
        float curBaseCycles = 0;
        float PPC = 0;

        if ((mAssignedSources.size() == 0) ||
            (mRotatedSrcCropBW != 0) ||
            ((mRotatedSrcCropBW == 0) &&
             ((src.transform & HAL_TRANSFORM_ROT_90) == 0))) {
            /* Just add cycles for current layer */
            if ((mAssignedSources.size() == 0) &&
                (display.displayIdentifier.id != UINT32_MAX) && (mMaxSrcLayerNum > 1)) {
                curBaseCycles = ((display.xres * display.yres) / G2D_BASE_PPC_COLORFILL);
                MPP_LOGD(eDebugCapacity, "There is no assigned layer. Colorfill cycles: %f should be added",
                         curBaseCycles);
            }
            curBaseCycles += getRequiredBaseCycles(src, dst);
            baseCycles += curBaseCycles;
            MPP_LOGD(eDebugCapacity, "mUsedBaseCycles was %f, Add base cycles %f, totalBaseCycle(%f)",
                     mUsedBaseCycles, curBaseCycles, baseCycles);
        } else {
            /* Recalculate cycles for all of layers */
            baseCycles = 0;
            MPP_LOGD(eDebugCapacity, "Check all of assigned layers cycles");
            /* PPC of layers that were added before should be changed */
            /* Check cycles of all assigned layers again */
            if ((display.displayIdentifier.id != UINT32_MAX) && (mMaxSrcLayerNum > 1)) {
                baseCycles += ((display.xres * display.yres) / G2D_BASE_PPC_COLORFILL);
                MPP_LOGD(eDebugCapacity, "colorfill cycles: %f, total cycles: %f",
                         ((display.xres * display.yres) / G2D_BASE_PPC_COLORFILL), cycles);
            }

            for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
                float assignedSrcCycles = 0;
                uint32_t assignedSrcResolution = mAssignedSources[i]->mSrcImg.w * mAssignedSources[i]->mSrcImg.h;
                uint32_t assignedDstResolution = mAssignedSources[i]->mMidImg.w * mAssignedSources[i]->mMidImg.h;
                uint32_t assignedMaxResolution = max(assignedSrcResolution, assignedDstResolution);
                float assignedPPC = getPPC(mAssignedSources[i]->mSrcImg, mAssignedSources[i]->mMidImg,
                                           mAssignedSources[i]->mSrcImg, &src, &dst);

                assignedSrcCycles = assignedMaxResolution / assignedPPC;
                baseCycles += assignedSrcCycles;

                MPP_LOGD(eDebugCapacity, "Src[%d] cycles: %f, total cycles: %f, PPC: %f, srcResolution: %d, dstResolution: %d, rot(%d)",
                         i, assignedSrcCycles, baseCycles, assignedPPC, assignedSrcResolution, assignedDstResolution, mAssignedSources[i]->mSrcImg.transform);
            }

            PPC = getPPC(src, dst, src, &src, &dst);

            srcCycles = maxResolution / PPC;
            baseCycles += srcCycles;

            MPP_LOGD(eDebugCapacity, "check mppSource cycles: %f, total cycles: %f, PPC: %f, srcResolution: %d, dstResolution: %d, rot(%d)",
                     srcCycles, baseCycles, PPC, srcResolution, dstResolution, src.transform);
        }

        capacity = baseCycles / getMPPClock();

        MPP_LOGD(eDebugCapacity, "baseCycles: %f, capacity: %f",
                 baseCycles, capacity);
    } else if (mPhysicalType == MPP_MSC) {
        /* Initialize value with the capacity that were already assigned */
        capacity = mUsedCapacity;

        /* Just add capacity for current layer */
        float srcPPC = getPPC(src, dst, src);
        float dstPPC = getPPC(src, dst, dst);
        float srcCapacity = ((float)(src.w * src.h)) / (getMPPClock() * srcPPC);
        float dstCapacity = ((float)(dst.w * dst.h)) / (getMPPClock() * dstPPC);

        capacity += max(srcCapacity, dstCapacity);

        if (mLogicalType == MPP_LOGICAL_MSC_COMBO &&
            (mMaxSrcLayerNum > 1) && (mAssignedSources.size() > 0) &&
            (is2StepBlendingRequired(mAssignedSources[0]->mSrcImg,
                                     mDstImgs[mCurrentDstBuf].bufferHandle)))
            capacity += max(srcCapacity, dstCapacity);

        MPP_LOGD(eDebugCapacity, "MSC capacity - src capa: %f, src.w: %d, src.h: %d, srcPPC: %f, dst capa: %f, dst.w: %d, dst.h: %d, dstPPC: %f",
                 srcCapacity, src.w, src.h, srcPPC, dstCapacity, dst.w, dst.h, dstPPC);
    }

    return capacity;
}

float ExynosMPP::getRequiredBaseCycles(struct exynos_image &src, struct exynos_image &dst) {
    if (mPhysicalType != MPP_G2D)
        return 0;

    uint32_t srcResolution = src.w * src.h;
    uint32_t dstResolution = dst.w * dst.h;
    uint32_t maxResolution = max(srcResolution, dstResolution);

    return maxResolution / (float)getPPC(src, dst, src);
}

bool ExynosMPP::addCapacity(ExynosMPPSource *mppSource) {
    if ((mppSource == NULL) || mCapacity == -1)
        return false;

    if (mPhysicalType == MPP_G2D) {
        bool needUpdateCapacity = true;
        if ((mAssignedSources.size() == 0) ||
            (mRotatedSrcCropBW != 0) ||
            ((mRotatedSrcCropBW == 0) &&
             ((mppSource->mSrcImg.transform & HAL_TRANSFORM_ROT_90) == 0))) {
            needUpdateCapacity = false;
        }

        if (needUpdateCapacity)
            return true;

        if ((mMaxSrcLayerNum > 1) &&
            (mAssignedSources.size() == 0)) {
            if (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) {
                /* This will be the first mppSource that is assigned to the ExynosMPP */
                /* Add capacity for background */
                mUsedBaseCycles += ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL);
                MPP_LOGD(eDebugCapacity, "\tcolorfill cycles: %f, total cycles: %f",
                         ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL), mUsedBaseCycles);
            } else {
                MPP_LOGE("mAssignedDisplay is not set");
            }
        }

        float baseCycles = getRequiredBaseCycles(mppSource->mSrcImg, mppSource->mMidImg);
        mUsedBaseCycles += baseCycles;

        uint32_t srcResolution = mppSource->mSrcImg.w * mppSource->mSrcImg.h;
        uint32_t dstResolution = mppSource->mMidImg.w * mppSource->mMidImg.h;
        if ((mppSource->mSrcImg.transform & HAL_TRANSFORM_ROT_90) == 0)
            mNoRotatedSrcCropBW += srcResolution;
        else
            mRotatedSrcCropBW += srcResolution;

        mUsedCapacity = mUsedBaseCycles / getMPPClock();

        MPP_LOGD(eDebugCapacity, "src num: %zu base cycle is added: %f, mUsedBaseCycles: %f, mUsedCapacity(%f), srcResolution: %d, dstResolution: %d, rot: %d, mNoRotatedSrcCropBW(%d), mRotatedSrcCropBW(%d)",
                 mAssignedSources.size(),
                 baseCycles, mUsedBaseCycles, mUsedCapacity, srcResolution, dstResolution,
                 mppSource->mSrcImg.transform, mNoRotatedSrcCropBW, mRotatedSrcCropBW);
    } else if (mPhysicalType == MPP_MSC) {
        mUsedCapacity = getRequiredCapacity(mAssignedDisplayInfo,
                                            mppSource->mSrcImg, mppSource->mMidImg);
    }

    return false;
}

bool ExynosMPP::removeCapacity(ExynosMPPSource *mppSource) {
    if ((mppSource == NULL) || (mCapacity == -1))
        return false;

    if (mPhysicalType == MPP_G2D) {
        uint32_t srcResolution = mppSource->mSrcImg.w * mppSource->mSrcImg.h;
        uint32_t dstResolution = mppSource->mDstImg.w * mppSource->mDstImg.h;

        uint32_t prevRotatedSrcCropBW = mRotatedSrcCropBW;

        if (mppSource->mSrcImg.transform == 0)
            mNoRotatedSrcCropBW -= srcResolution;
        else
            mRotatedSrcCropBW -= srcResolution;

        if ((prevRotatedSrcCropBW > 0) && (mRotatedSrcCropBW == 0))
            return true;

        float baseCycles = getRequiredBaseCycles(mppSource->mSrcImg, mppSource->mMidImg);
        mUsedBaseCycles -= baseCycles;

        mUsedCapacity = mUsedBaseCycles / getMPPClock();

        MPP_LOGD(eDebugCapacity, "src num: %zu, base cycle is removed: %f, mUsedBaseCycles: %f, mUsedCapacity(%f), srcResolution: %d, dstResolution: %d, rot: %d, mNoRotatedSrcCropBW(%d), mRotatedSrcCropBW(%d)",
                 mAssignedSources.size(),
                 baseCycles, mUsedBaseCycles, mUsedCapacity, srcResolution, dstResolution,
                 mppSource->mSrcImg.transform, mNoRotatedSrcCropBW, mRotatedSrcCropBW);
    } else if (mPhysicalType == MPP_MSC) {
        exynos_image &src = mppSource->mSrcImg;
        exynos_image &dst = mppSource->mDstImg;
        uint32_t srcResolution = src.w * src.h;
        uint32_t dstResolution = dst.w * dst.h;

        float srcCapacity = (float)srcResolution / getPPC(src, dst, src);
        float dstCapacity = (float)dstResolution / getPPC(src, dst, dst);

        mUsedCapacity -= max(srcCapacity, dstCapacity);
    }

    return false;
}

void ExynosMPP::resetUsedCapacity() {
    mUsedCapacity = 0;
    mUsedBaseCycles = 0;
    mRotatedSrcCropBW = 0;
    mNoRotatedSrcCropBW = 0;
}

int32_t ExynosMPP::updateUsedCapacity() {
    int32_t ret = NO_ERROR;
    if (mCapacity == -1)
        return ret;

    float capacity = 0;
    mUsedCapacity = 0;

    mRotatedSrcCropBW = 0;
    mNoRotatedSrcCropBW = 0;

    if ((mPhysicalType == MPP_G2D) &&
        (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
        (mAssignedSources.size() > 0)) {
        float cycles = 0;

        if (mMaxSrcLayerNum > 1) {
            cycles += ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL);
            MPP_LOGD(eDebugCapacity, "\tcolorfill cycles: %f, total cycles: %f",
                     ((mAssignedDisplayInfo.xres * mAssignedDisplayInfo.yres) / G2D_BASE_PPC_COLORFILL), cycles);
        }
        for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
            uint32_t srcResolution = mAssignedSources[i]->mSrcImg.w * mAssignedSources[i]->mSrcImg.h;
            if ((mAssignedSources[i]->mSrcImg.transform & HAL_TRANSFORM_ROT_90) == 0)
                mNoRotatedSrcCropBW += srcResolution;
            else
                mRotatedSrcCropBW += srcResolution;
        }
        MPP_LOGD(eDebugCapacity, "mNoRotatedSrcCropBW(%d), mRotatedSrcCropBW(%d)",
                 mNoRotatedSrcCropBW, mRotatedSrcCropBW);
        for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
            float srcCycles = 0;
            uint32_t srcResolution = mAssignedSources[i]->mSrcImg.w * mAssignedSources[i]->mSrcImg.h;
            uint32_t dstResolution = mAssignedSources[i]->mMidImg.w * mAssignedSources[i]->mMidImg.h;
            uint32_t maxResolution = max(srcResolution, dstResolution);
            float PPC = getPPC(mAssignedSources[i]->mSrcImg, mAssignedSources[i]->mMidImg, mAssignedSources[i]->mSrcImg);
            srcCycles = maxResolution / PPC;
            cycles += srcCycles;

            MPP_LOGD(eDebugCapacity, "Src[%d] cycles: %f, total cycles: %f, PPC: %f, srcResolution: %d, dstResolution: %d, rot(%d)",
                     i, srcCycles, cycles, PPC, srcResolution, dstResolution, mAssignedSources[i]->mSrcImg.transform);
        }

        mUsedBaseCycles = cycles;
        capacity = cycles / getMPPClock();

        mUsedCapacity = capacity;
    }
    MPP_LOGD(eDebugCapacity, "assigned layer size(%zu), mUsedCapacity: %f", mAssignedSources.size(), mUsedCapacity);

    return mUsedCapacity;
}

uint32_t ExynosMPP::getMPPClock() {
    if (mPhysicalType == MPP_G2D)
        return G2D_CLOCK;
    else if (mPhysicalType == MPP_MSC)
        return MSC_CLOCK;
    else
        return 0;
}

uint32_t ExynosMPP::getRestrictionClassification(struct exynos_image &img) {
    return !!(img.exynosFormat.isRgb() == false);
}

int ExynosMPP::prioritize(int priority) {
    if ((mPhysicalType != MPP_G2D) ||
        (mAcrylicHandle == NULL)) {
        MPP_LOGE("invalid function call");
        return -1;
    }
    int ret = NO_ERROR;
    ret = mAcrylicHandle->prioritize(priority);

    if ((priority > 0) && (ret == 1)) {
        /* G2D Driver returned EBUSY */
        mHWBusyFlag = true;
    }
    MPP_LOGD(eDebugMPP, "set resource prioritize (%d), ret(%d), mHWBusyFlag(%d)", priority, ret, mHWBusyFlag);

    return ret;
}

uint32_t ExynosMPP::increaseDstBuffIndex() {
    if (mAllocOutBufFlag)
        mCurrentDstBuf = (mCurrentDstBuf + 1) % NUM_MPP_DST_BUFS(mLogicalType);
    return mCurrentDstBuf;
}

void ExynosMPP::reloadResourceForHWFC() {
    ALOGI("reloadResourceForHWFC()");
    if (mAcrylicHandle != NULL)
        delete mAcrylicHandle;
    mAcrylicHandle = AcrylicFactory::createAcrylic("default_compositor");
    if (mAcrylicHandle == NULL) {
        MPP_LOGE("Fail to allocate compositor");
    } else {
        mAcrylicHandle->setDefaultColor(0, 0, 0, 0);
        MPP_LOGI("The resource is reloaded for HWFC: %p", mAcrylicHandle);
    }
    for (uint32_t i = 0; i < NUM_MPP_SRC_BUFS; i++) {
        if (mSrcImgs[i].mppLayer != NULL) {
            delete mSrcImgs[i].mppLayer;
            mSrcImgs[i].mppLayer = NULL;
        }
    }
}

void ExynosMPP::setTargetDisplayLuminance(uint16_t min, uint16_t max) {
    MPP_LOGD(eDebugMPP, "%s: min(%d), max(%d)", __func__, min, max);
    if (mAcrylicHandle == NULL) {
        MPP_LOGE("mAcrylicHandle is NULL");
    } else
        mAcrylicHandle->setTargetDisplayLuminance(min, max);
}

void ExynosMPP::setTargetDisplayDevice(int device) {
    ALOGI("%s: device(%d)", __func__, device);
    if (mAcrylicHandle == NULL) {
        MPP_LOGE("mAcrylicHandle is NULL");
    } else
        mAcrylicHandle->setTargetDisplayInfo(&device);
}

void ExynosMPP::dump(String8 &result) {
    int32_t assignedDisplayType = -1;
    if (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX)
        assignedDisplayType = mAssignedDisplayInfo.displayIdentifier.type;

    result.appendFormat("%s: types mppType(%d), (p:%d, l:0x%2x), indexs(p:%d, l:%d), preAssignDisplay(0x%2x)\n",
                        mName.c_str(), mMPPType, mPhysicalType, mLogicalType, mPhysicalIndex, mLogicalIndex, mPreAssignDisplayInfo);
    result.appendFormat("\tEnable(by debug): %d, Disable(by scenario):0x%8x, HWState: %d, AssignedState: %d, assignedDisplay(%d)\n",
                        mEnableByDebug, mDisableByUserScenario, mHWState, mAssignedState, assignedDisplayType);
    result.appendFormat("\tPrevAssignedState: %d, PrevAssignedDisplayType: %d, ReservedDisplay: %d\n",
                        mPrevAssignedState, mPrevAssignedDisplayType, mReservedDisplayInfo.displayIdentifier.id);
    result.appendFormat("\tassinedSourceNum(%zu), Capacity(%f), CapaUsed(%f), mCurrentDstBuf(%d)\n",
                        mAssignedSources.size(), mCapacity, mUsedCapacity, mCurrentDstBuf);
}

void ExynosMPP::dumpBufInfo(String8 &str) {
    uint32_t index = 0;
    size_t bufLength[MAX_HW2D_PLANES];
    for (auto &src : mAssignedSources) {
        String8 dumpStr;
        src->dump(dumpStr);
        str.appendFormat("src[%d] %s\n", index++, dumpStr.c_str());

        if (getBufLength(src->mSrcImg.bufferHandle, MAX_HW2D_PLANES, bufLength,
                         src->mSrcImg.exynosFormat.halFormat(), src->mSrcImg.fullWidth,
                         src->mSrcImg.fullHeight) != NO_ERROR) {
            str.appendFormat("\tgetBufLength fail\n");
            continue;
        }
        str.appendFormat("\tformat(%s), BufLength(%zu, %zu, %zu)\n",
                         src->mSrcImg.exynosFormat.name().c_str(),
                         bufLength[0], bufLength[1], bufLength[2]);
    }

    str.appendFormat("dst mXres: %d, mYres: %d, mCurrentDstBuf(%d)",
                     mAssignedDisplayInfo.xres, mAssignedDisplayInfo.yres,
                     mCurrentDstBuf);
    buffer_handle_t dstHandle = mDstImgs[mCurrentDstBuf].bufferHandle;
    ExynosGraphicBufferMeta gmeta(dstHandle);
    if (getBufLength(dstHandle, MAX_HW2D_PLANES, bufLength,
                     mDstImgs[mCurrentDstBuf].format.halFormat(), gmeta.stride,
                     gmeta.vstride) != NO_ERROR) {
        str.appendFormat("\tgetBufLength fail\n");
        return;
    }
    str.appendFormat("\tformat(%s), BufLength(%zu, %zu, %zu)\n",
                     mDstImgs[mCurrentDstBuf].format.name().c_str(),
                     bufLength[0], bufLength[1], bufLength[2]);
}

void ExynosMPP::closeFences() {
    for (uint32_t i = 0; i < mAssignedSources.size(); i++) {
        mSrcImgs[i].acrylicAcquireFenceFd =
            mFenceTracer.fence_close(mSrcImgs[i].acrylicAcquireFenceFd,
                                     mAssignedDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_G2D,
                                     "mpp::closeFences: src acrylicAcquireFence");
        mSrcImgs[i].acrylicReleaseFenceFd =
            mFenceTracer.fence_close(mSrcImgs[i].acrylicReleaseFenceFd,
                                     mAssignedDisplayInfo.displayIdentifier,
                                     FENCE_TYPE_SRC_RELEASE, FENCE_IP_G2D,
                                     "mpp::closeFences: src acrylicReleaseFence");
    }

    mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd =
        mFenceTracer.fence_close(mDstImgs[mCurrentDstBuf].acrylicAcquireFenceFd,
                                 mAssignedDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_DST_ACQUIRE, FENCE_IP_G2D,
                                 "mpp::closeFences: dst acrylicAcquireFence");
    mDstImgs[mCurrentDstBuf].acrylicReleaseFenceFd =
        mFenceTracer.fence_close(mDstImgs[mCurrentDstBuf].acrylicReleaseFenceFd,
                                 mAssignedDisplayInfo.displayIdentifier,
                                 FENCE_TYPE_DST_RELEASE, FENCE_IP_G2D,
                                 "mpp::closeFences: dst acrylicReleaseFence");
}

void ExynosMPP::addFormatRestrictions(restriction_key table) {
    mFormatRestrictions.push_back(table);
    HDEBUGLOGD(eDebugAttrSetting, "MPP : %s, %d, %s, %d",
               mName.c_str(),
               mFormatRestrictions.back().nodeType,
               getFormatStr(mFormatRestrictions.back().format).c_str(),
               mFormatRestrictions.back().reserved);
}

void ExynosMPP::addSizeRestrictions(restriction_size srcSize, restriction_size dstSize, restriction_classification format) {
    mSrcSizeRestrictions[format] = srcSize;
    mDstSizeRestrictions[format] = dstSize;

    HDEBUGLOGD(eDebugAttrSetting, "MPP : %s: Src: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
               mName.c_str(),
               srcSize.maxDownScale,
               srcSize.maxUpScale,
               srcSize.maxFullWidth,
               srcSize.maxFullHeight,
               srcSize.minFullWidth,
               srcSize.minFullHeight,
               srcSize.fullWidthAlign,
               srcSize.fullHeightAlign,
               srcSize.maxCropWidth,
               srcSize.maxCropHeight,
               srcSize.minCropWidth,
               srcSize.minCropHeight,
               srcSize.cropXAlign,
               srcSize.cropYAlign,
               srcSize.cropWidthAlign,
               srcSize.cropHeightAlign);
    HDEBUGLOGD(eDebugAttrSetting, "MPP : %s: Dst: %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
               mName.c_str(),
               dstSize.maxDownScale,
               dstSize.maxUpScale,
               dstSize.maxFullWidth,
               dstSize.maxFullHeight,
               dstSize.minFullWidth,
               dstSize.minFullHeight,
               dstSize.fullWidthAlign,
               dstSize.fullHeightAlign,
               dstSize.maxCropWidth,
               dstSize.maxCropHeight,
               dstSize.minCropWidth,
               dstSize.minCropHeight,
               dstSize.cropXAlign,
               dstSize.cropYAlign,
               dstSize.cropWidthAlign,
               dstSize.cropHeightAlign);
}

void ExynosMPP::printMppsAttr() {
    restriction_classification format;
    format = RESTRICTION_RGB;

    ALOGD("%s restriction info start --------", mName.c_str());

    ALOGD("1. support format");
    for (auto r : mFormatRestrictions) {
        ALOGD("node type: %d, format: %s, reserved: %d",
              r.nodeType, getFormatStr(r.format).c_str(), r.reserved);
    }

    ALOGD("2. size restriction");
    for (uint32_t i = 0; i < RESTRICTION_MAX; i++) {
        restriction_size srcSize = mSrcSizeRestrictions[i];
        restriction_size dstSize = mDstSizeRestrictions[i];
        if (i == RESTRICTION_RGB) {
            ALOGD("RGB format restriction");
        } else if (i == RESTRICTION_YUV) {
            ALOGD("YUV format restriction");
        }
        ALOGD("Src: maxDownSacle: %d, maxUpSacle: %d,"
              "maxFullWidth: %d, maxFullHeight: %d,"
              "minFullWidth: %d, minFullHeight: %d,"
              "fullWidthAlign: %d, fullHeightAlign: %d,"
              "maxCropWidth: %d, maxCropHeight: %d,"
              "minCropWidth: %d, minCropHeight: %d,"
              "cropXAlign: %d, cropYAlign: %d,"
              "cropWidthAlign: %d, cropHeightAlign: %d",
              srcSize.maxDownScale,
              srcSize.maxUpScale,
              srcSize.maxFullWidth,
              srcSize.maxFullHeight,
              srcSize.minFullWidth,
              srcSize.minFullHeight,
              srcSize.fullWidthAlign,
              srcSize.fullHeightAlign,
              srcSize.maxCropWidth,
              srcSize.maxCropHeight,
              srcSize.minCropWidth,
              srcSize.minCropHeight,
              srcSize.cropXAlign,
              srcSize.cropYAlign,
              srcSize.cropWidthAlign,
              srcSize.cropHeightAlign);
        ALOGD("Dst: maxDownSacle: %d, maxUpSacle: %d,"
              "maxFullWidth: %d, maxFullHeight: %d,"
              "minFullWidth: %d, minFullHeight: %d,"
              "fullWidthAlign: %d, fullHeightAlign: %d,"
              "maxCropWidth: %d, maxCropHeight: %d,"
              "minCropWidth: %d, minCropHeight: %d,"
              "cropXAlign: %d, cropYAlign: %d,"
              "cropWidthAlign: %d, cropHeightAlign: %d",
              dstSize.maxDownScale,
              dstSize.maxUpScale,
              dstSize.maxFullWidth,
              dstSize.maxFullHeight,
              dstSize.minFullWidth,
              dstSize.minFullHeight,
              dstSize.fullWidthAlign,
              dstSize.fullHeightAlign,
              dstSize.maxCropWidth,
              dstSize.maxCropHeight,
              dstSize.minCropWidth,
              dstSize.minCropHeight,
              dstSize.cropXAlign,
              dstSize.cropYAlign,
              dstSize.cropWidthAlign,
              dstSize.cropHeightAlign);
    }

    ALOGD("%s restriction info end --------", mName.c_str());
}

void ExynosMPP::updatePPCTable(ppc_table &map) {
    ppc_table_map = map;
}
