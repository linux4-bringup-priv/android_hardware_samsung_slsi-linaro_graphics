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

#include "ExynosMPPModule.h"
#include "ExynosHWCDebug.h"
#include "ExynosVirtualDisplay.h"

ExynosMPPModule::ExynosMPPModule(uint32_t physicalType, uint32_t logicalType, const char *name,
        uint32_t physicalIndex, uint32_t logicalIndex,
        uint32_t preAssignInfo, uint32_t mppType)
    : ExynosMPP(physicalType, logicalType, name, physicalIndex, logicalIndex, preAssignInfo, mppType)
{
    const vOtfInfo_t *info = std::find_if(std::begin(VOTF_INFO_MAP),
            std::end(VOTF_INFO_MAP), [=](auto &it){
            return ((it.type == mPhysicalType) && (it.index == mPhysicalIndex));
            });

    if (info != std::end(VOTF_INFO_MAP)) {
        mVotfInfo.dmaIndex = info->dma_idx;
        mVotfInfo.trsIndex = info->trs_idx;
    }
}

ExynosMPPModule::~ExynosMPPModule()
{
}

uint32_t ExynosMPPModule::getDstWidthAlign(struct exynos_image &dst)
{
    if (((dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (dst.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B)) &&
        (mPhysicalType == MPP_MSC))
        return 4;

    return  ExynosMPP::getDstWidthAlign(dst);
}

bool ExynosMPPModule::isSupportedCompression(struct exynos_image &src)
{
    /* TODO : add 4K, 2K restriction here for DPPs */
    return ExynosMPP::isSupportedCompression(src);
}

bool ExynosMPPModule::isSupportedTransform(struct exynos_image &src)
{
    switch (mPhysicalType)
    {
    case MPP_MSC:
    case MPP_G2D:
        return true;
    case MPP_DPP_G:
    case MPP_DPP_GF:
    case MPP_DPP_VG:
    case MPP_DPP_VGS:
    case MPP_DPP_VGF:
    case MPP_DPP_VGFS:
        /* If it's not a roatation, flip is allowed */
        if ((src.transform & HAL_TRANSFORM_ROT_90) == 0)
        {
            /* but flip is not allowed for SBWC, AFBC */
            if ((src.exynosFormat.isSBWC() || ((src.compressionInfo.type == COMP_TYPE_AFBC) || (src.compressionInfo.type == COMP_TYPE_SBWC)))
                && (src.transform != 0)) {
                return false;
            } else {
                return true;
            }
        } else {
            return false;
        }
    case MPP_DPP_VGRFS:
        /* Flip is not allowed for SBWC, AFBC. but rotation is allowed. */
        if ((src.transform & HAL_TRANSFORM_ROT_90) == 0) {
            if ((src.exynosFormat.isSBWC() || ((src.compressionInfo.type == COMP_TYPE_AFBC) || (src.compressionInfo.type == COMP_TYPE_SBWC)))
                && (src.transform != 0)) {
                return false;
            }
        }
        if (src.exynosFormat.isYUV420()) {
            return true;
        } else { /* RGB case */
            if ((src.transform & HAL_TRANSFORM_ROT_90) == 0)
            {
                return true;
            } else {
                return false;
            }
        }
    default:
            return true;
    }
}

uint32_t ExynosMPPModule::getSrcMaxCropSize(struct exynos_image &src)
{

    if ((mLogicalType == MPP_LOGICAL_DPP_VGS8K) ||
            (mLogicalType == MPP_LOGICAL_DPP_VGFS8K) ||
            (mLogicalType == MPP_LOGICAL_DPP_VGRFS8K))
        return MAX_DPP_8K_SRC_SIZE;

    if ((mPhysicalType == MPP_DPP_VGRFS) &&
            (src.transform & HAL_TRANSFORM_ROT_90))
        return MAX_DPP_ROT_SRC_SIZE;
    else
        return ExynosMPP::getSrcMaxCropSize(src);

    return ExynosMPP::getSrcMaxCropSize(src);
}

uint32_t ExynosMPPModule::getSrcMaxCropWidth(struct exynos_image &src)
{
    if ((mPhysicalType == MPP_DPP_VGFS) && (src.compressionInfo.type == COMP_TYPE_AFBC))
        return VGFS_AFBC_WIDTH_LIMIT;
    else
        return ExynosMPP::getSrcMaxCropWidth(src);
}

uint32_t ExynosMPPModule::getSrcMaxCropHeight(struct exynos_image &src)
{
    if ((mLogicalType == MPP_LOGICAL_DPP_VGS8K) ||
            (mLogicalType == MPP_LOGICAL_DPP_VGFS8K) ||
            (mLogicalType == MPP_LOGICAL_DPP_VGRFS8K)) {
        /* Height limitation is 7680 for 8K rotation */
        if (src.transform & HAL_TRANSFORM_ROT_90)
            return VIRTUAL_8K_WIDTH;
    }

    if ((mMPPType == MPP_TYPE_OTF) &&
        (src.transform & HAL_TRANSFORM_ROT_90))
        return 2160;

    uint32_t idx = getRestrictionClassification(src);
    return mSrcSizeRestrictions[idx].maxCropHeight;
}

bool ExynosMPPModule::hasEnoughCapa(DisplayInfo &display, struct exynos_image &src,
        struct exynos_image &dst, float totalUsedCapa)
{
    bool ret = false;
    float capacity = mCapacity;
    if ((mLogicalType == MPP_LOGICAL_G2D_COMBO) &&
        (display.displayIdentifier.type == HWC_DISPLAY_VIRTUAL) &&
        (display.isWFDState == LLWFD))
        mCapacity = LLWFD_G2D_CAPACITY;
    ret = ExynosMPP::hasEnoughCapa(display, src, dst, totalUsedCapa);
    mCapacity = capacity;
    return ret;
}

bool ExynosMPPModule::isSupportedCapability(DisplayInfo &display, struct exynos_image &src)
{
    return true;
}

bool ExynosMPPModule::isSharedMPPUsed()
{
    /*
     * In case of current MPP is virtual 8K MPP.
     * If subMPPs are used then current MPP is forbidden
     */
    if (isVirtual8KOtf()) {
        if ((mSubMPP[0] == nullptr) || (mSubMPP[1] == nullptr)) {
            MPP_LOGE("Invalid virtual MPP, subMPP0(%p), subMPP1(%p)",
                    mSubMPP[0], mSubMPP[1]);
            /* Return true in order not to use virtual mpp */
            return true;
        }
        /* Check subMPP */
        if ((mSubMPP[0]->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) ||
            (mSubMPP[1]->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED)) {
            HDEBUGLOGD(eDebugMPP, "Current MPP is virtaul 8K(%s), "
                    "but sub MPP(%s: %d, %s: %d) is already assigned..",
                    this->mName.c_str(), mSubMPP[0]->mName.c_str(),
                    mSubMPP[0]->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED,
                    mSubMPP[1]->mName.c_str(),
                    mSubMPP[1]->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED);
            return true;
        }
        return false;
    }

    /*
     * In case of current MPP is sub-sharedMPPs.
     * If virtual 8K MPP is used then current MPP is forbidden
     */
    if ((mVirtual8KMPP != nullptr) &&
        (mVirtual8KMPP->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED)) {
        HDEBUGLOGD(eDebugMPP, "Current MPP is virtaul 8K(%s), "
                "but sub MPP(%s) is already assigned..",
                this->mName.c_str(), mVirtual8KMPP->mName.c_str());
        return true;
    }

    return false;
}

int32_t ExynosMPPModule::setVotfLayerData(exynos_mpp_img_info *srcImgInfo)
{
    if (mPhysicalType == MPP_MSC) {
        HDEBUGLOGD(eDebugMPP, "%s, %d, %d, %d, %d", __func__,
                mVotfInfo.enable, mVotfInfo.dmaIndex, mVotfInfo.trsIndex,
                mVotfInfo.bufIndex);
    }

    if ((mPhysicalType != MPP_MSC) || !mVotfInfo.enable)
        return NO_ERROR;

    /*
     * First parameter(void *) of setLayerData shouldn't be destroyed before
     * libacryl addresses the current frame in Acrylic::execute.
     */
    srcImgInfo->mppLayer->setLayerData(&mVotfInfo.data, sizeof(mVotfInfo.data));

    return NO_ERROR;
}

bool ExynosMPPModule::canUseVotf(struct exynos_image &src)
{
    if (mPhysicalType != MPP_MSC)
        return false;

    if (((src.transform & HAL_TRANSFORM_ROT_90) == 0) &&
        (src.w == 7680)) {
        return true;
    }

    return false;
}

bool ExynosMPPModule::isCapacityExceptionCondition(float totalUsedCapacity, float requiredCapacity, struct exynos_image &src)
{
    if ((hasHdrInfo(src) &&
        (totalUsedCapacity == 0) &&
        (requiredCapacity < (mCapacity * MPP_HDR_MARGIN)))) {
        return true;
    } else if (canUseVotf(src)) {
        return true;
    } else {
        return false;
    }
}

uint32_t ExynosMPPModule::getMaxDownscale(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst)
{
    if (canUseVotf(src))
        return 4;
    return ExynosMPP::getMaxDownscale(display, src, dst);
}

bool ExynosMPPModule::scaleAllowedByDPPPerformance(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst)
{
    bool isPerpendicular = !!(dst.transform & HAL_TRANSFORM_ROT_90);
    float scaleRatio_H = 1;
    float scaleRatio_V = 1;
    if (isPerpendicular) {
        scaleRatio_H = (float)src.w/(float)dst.h;
        scaleRatio_V = (float)src.h/(float)dst.w;
    } else {
        scaleRatio_H = (float)src.w/(float)dst.w;
        scaleRatio_V = (float)src.h/(float)dst.h;
    }

    float dstW = (float)dst.w;
    float displayW = (float)display.xres;
    float displayH = (float)display.yres;

    uint32_t vsyncPeriod = display.workingVsyncPeriod;
    int fps = (int)(1000000000 / vsyncPeriod);

    float vppResolClockFactor = fps * VPP_MARGIN;
    float resolClock = displayW * displayH * vppResolClockFactor;

    if ((mPhysicalType == MPP_DPP_VGS) ||
        (mPhysicalType == MPP_DPP_VGFS) ||
        (mPhysicalType == MPP_DPP_VGRFS)) {
        if (scaleRatio_H > 2 || scaleRatio_V > 2) {
            if ((float)VPP_CLOCK < ((resolClock * scaleRatio_H * scaleRatio_V * VPP_DISP_FACTOR)/VPP_PIXEL_PER_CLOCK))
                return false;
        } else {
            if ((float)VPP_CLOCK < ((resolClock * scaleRatio_H * scaleRatio_V * VPP_DISP_FACTOR)/VPP_PIXEL_PER_CLOCK * (dstW/displayW)))
                return false;
        }
    }
    return true;
}
