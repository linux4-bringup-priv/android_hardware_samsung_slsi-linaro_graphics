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
#include <hardware/exynos/hdrInterface.h>

ExynosMPPModule::ExynosMPPModule(uint32_t physicalType, uint32_t logicalType,
        const char *name, uint32_t physicalIndex, uint32_t logicalIndex,
        uint32_t preAssignInfo, uint32_t mppType)
    : ExynosMPP(physicalType, logicalType, name, physicalIndex, logicalIndex, preAssignInfo, mppType)
{
}

ExynosMPPModule::~ExynosMPPModule()
{
}

uint32_t ExynosMPPModule::getMPPClock()
{
    if (mPhysicalType == MPP_G2D)
        return 667000;
    else
        return 0;
}

bool ExynosMPPModule::isDataspaceSupportedByMPP(struct exynos_image &src, struct exynos_image &dst)
{
    if ((mLogicalType == MPP_LOGICAL_G2D_YUV) &&
        (hasHdrInfo(src)) && (dst.exynosFormat == HAL_PIXEL_FORMAT_YCBCR_P010) &&
        (src.dataSpace == dst.dataSpace))
        return false;
    return ExynosMPP::isDataspaceSupportedByMPP(src, dst);
}

#ifdef USE_HDR_INTERFACE
bool ExynosMPPModule::isQualifiedHDRPipeRestriction(DisplayInfo &display,
        struct exynos_image &src, struct exynos_image &dst)
{
    bool ret = true;
    if (mPhysicalType == MPP_G2D) {
        android_dataspace_t src_dataSpace = src.dataSpace;
        android_dataspace_t dst_dataSpace = getDstDataspace(dst.exynosFormat.halFormat(), display, dst.dataSpace);
        int hdrError = 0;
        struct hdrCoef output[MPP_G2D_HDR_PIPE_NUM];
        int hdr_map[MPP_G2D_HDR_PIPE_NUM];

        /* If G2D doesn't support WCG and color mode is NATIVE,
         * G2D can composit layers without WCG processing.
         * So we will return true forcely if it meets the condition. */
        if ((mMaxSrcLayerNum > 1) && ((mAttr & MPP_ATTR_WCG) == 0) &&
            (display.colorMode == HAL_COLOR_MODE_NATIVE))
            return true;

        memset(&output, 0, sizeof(struct hdrCoef) * MPP_G2D_HDR_PIPE_NUM);
        memset(&hdr_map, 0, sizeof(int) * MPP_G2D_HDR_PIPE_NUM);

        if (hasHdrInfo(src) && src.metaParcel)
            mAssignedLuminance.push(src.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance/10000);
        else
            mAssignedLuminance.push(0);

        /* Google gave guide that HAL_DATSPACE_UNKNOWN should trigger
           the same behavior in HWC as HAL_DATASPACE_V0_SRGB. (11251035)
           So we modified data space from UNKNOWN to V0_SRGB */
        if (src_dataSpace == HAL_DATASPACE_UNKNOWN) {
            if (src.exynosFormat.isRgb())
                src_dataSpace = HAL_DATASPACE_V0_SRGB;
            else
                src_dataSpace = HAL_DATASPACE_V0_BT601_625;
        }
        mAssignedDataspace.push(src_dataSpace);
        if (dst_dataSpace == HAL_DATASPACE_UNKNOWN)
            dst_dataSpace = HAL_DATASPACE_V0_SRGB;

        if ((hdrError = mHdrCoefInterface->getHdrCoef(
                    const_cast<android_dataspace *>(mAssignedDataspace.array()),
                    const_cast<int *>(mAssignedLuminance.array()),
                    mAssignedDataspace.size(), dst_dataSpace, 0,
                    output, hdr_map)) < 0) {
            ret = false;
            /* TODO: Need to change to check enum */
            if (hdrError != -2)
                MPP_LOGE("%s:: getHdrCoef error(%d)", __func__, hdrError);
            MPP_LOGD(eDebugMPP, "getHdrCoef ret(%d)", hdrError);
        }

        mAssignedDataspace.pop();
        mAssignedLuminance.pop();
    }
    return ret;
}

int32_t ExynosMPPModule::resetMPP()
{
    int32_t ret = ExynosMPP::resetMPP();
    if (mPhysicalType == MPP_G2D) {
        mAssignedDataspace.clear();
        mAssignedLuminance.clear();
    }
    return ret;
}

int32_t ExynosMPPModule::resetAssignedState()
{
    int32_t ret = ExynosMPP::resetAssignedState();
    for (int i = (int)mAssignedSources.size(); i-- > 0;) {
        if (mPhysicalType == MPP_G2D) {
            mAssignedDataspace.removeItemsAt(i);
            mAssignedLuminance.removeItemsAt(i);
        }
    }
    return ret;
}

int32_t ExynosMPPModule::resetAssignedState(ExynosMPPSource *mppSource)
{
    int32_t ret = ExynosMPP::resetAssignedState(mppSource);
    for (int i = (int)mAssignedSources.size(); i-- > 0;) {
        if (mAssignedSources[i] == mppSource) {
            if (mPhysicalType == MPP_G2D) {
                mAssignedDataspace.removeItemsAt(i);
                mAssignedLuminance.removeItemsAt(i);
            }
        }
    }
    return ret;
}

bool ExynosMPPModule::isAssignableState(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst)
{
    bool ret = ExynosMPP::isAssignableState(display, src, dst);
    if (ret)
        return isQualifiedHDRPipeRestriction(display, src, dst);
    else
        return false;
}

int32_t ExynosMPPModule::setG2DColorConversionInfo()
{
    int32_t ret = 0;
    if (mPhysicalType == MPP_G2D) {
        struct hdrCoef output[MPP_G2D_HDR_PIPE_NUM];
        int hdr_map[MPP_G2D_HDR_PIPE_NUM];
        int hdrError = 0;
        int sourceNum = mAssignedSources.size();
        memset(&output, 0, sizeof(struct hdrCoef) * MPP_G2D_HDR_PIPE_NUM);
        memset(&hdr_map, 0, sizeof(int) * MPP_G2D_HDR_PIPE_NUM);
        Vector<android_dataspace> assignedDataspace;
        Vector<int> assignedLuminance;
        android_dataspace_t dst_dataSpace = mDstImgs[mCurrentDstBuf].dataspace;
        String8 log;

        /* If G2D doesn't support WCG and color mode is NATIVE,
         * G2D can composit layers without WCG processing.
         * So we will return true forcely if it meets the condition. */
        if ((mMaxSrcLayerNum > 1) && ((mAttr & MPP_ATTR_WCG) == 0) &&
            (mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
            (mAssignedDisplayInfo.colorMode == HAL_COLOR_MODE_NATIVE)) {
            mAcrylicHandle->clearLibHdrCoefficient();
            return ret;
        }

        if (sourceNum > 1) {
            assignedDataspace.add(HAL_DATASPACE_V0_SRGB);
            assignedLuminance.add(0);
        }

        for (size_t i = 0; i < sourceNum; i++) {
            android_dataspace src_dataSpace = mAssignedSources[i]->mSrcImg.dataSpace;
            /* Google gave guide that HAL_DATSPACE_UNKNOWN should trigger
               the same behavior in HWC as HAL_DATASPACE_V0_SRGB. (11251035)
               So we modified data space from UNKNOWN to V0_SRGB */
            if (src_dataSpace == HAL_DATASPACE_UNKNOWN) {
                if (mAssignedSources[i]->mSrcImg.exynosFormat.isRgb())
                    src_dataSpace = HAL_DATASPACE_V0_SRGB;
                else
                    src_dataSpace = HAL_DATASPACE_V0_BT601_625;
            }
            assignedDataspace.add(src_dataSpace);
            if (hasHdrInfo(mAssignedSources[i]->mSrcImg) &&
                mAssignedSources[i]->mSrcImg.metaParcel)
                assignedLuminance.add(mAssignedSources[i]->mSrcImg.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance/10000);
            else
                assignedLuminance.add(0);
        }
        if (dst_dataSpace == HAL_DATASPACE_UNKNOWN)
            dst_dataSpace = HAL_DATASPACE_V0_SRGB;

        if ((hdrError = mHdrCoefInterface->getHdrCoef(
                    const_cast<android_dataspace *>(assignedDataspace.array()),
                    const_cast<int *>(assignedLuminance.array()),
                    assignedDataspace.size(), dst_dataSpace, 0,
                    output, hdr_map)) == NO_ERROR) {
            /* Propagate hdr coef to libacryl */
            mAcrylicHandle->setLibHdrCoefficient(hdr_map, output);
        } else {
            if (hdrError < 0)
                MPP_LOGE("%s:: getHdrCoef error(%d)", __func__, hdrError);
            /*
             * It is error case if the return value is minus.
             * There is no layer that needs conversion
             * if return value is positive number.
             * Clear hdr coef setting for libacryl in those cases
             */
            /* Clear hdr coef setting to libacryl */
            mAcrylicHandle->clearLibHdrCoefficient();
        }
    }
    return ret;
}

int32_t ExynosMPPModule::assignMPP(DisplayInfo &display, ExynosMPPSource *mppSource)
{
    int32_t ret = ExynosMPP::assignMPP(display, mppSource);
    if (mPhysicalType == MPP_G2D) {
        android_dataspace src_dataSpace = mppSource->mSrcImg.dataSpace;
        if ((mNeedSolidColorLayer == true) && (mAssignedDataspace.size() == 0)) {
            mAssignedDataspace.add(HAL_DATASPACE_V0_SRGB);
            mAssignedLuminance.add(0);
        }
        /* Google gave guide that HAL_DATSPACE_UNKNOWN should trigger
           the same behavior in HWC as HAL_DATASPACE_V0_SRGB. (11251035)
           So we modified data space from UNKNOWN to V0_SRGB */
        if (src_dataSpace == HAL_DATASPACE_UNKNOWN) {
            if (mppSource->mSrcImg.exynosFormat.isRgb())
                src_dataSpace = HAL_DATASPACE_V0_SRGB;
            else
                src_dataSpace = HAL_DATASPACE_V0_BT601_625;
        }
        mAssignedDataspace.add(src_dataSpace);
        if (hasHdrInfo(mppSource->mSrcImg) && mppSource->mSrcImg.metaParcel)
            mAssignedLuminance.add(mppSource->mSrcImg.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance/10000);
        else
            mAssignedLuminance.add(0);
    }
    return ret;
}

int32_t ExynosMPPModule::doPostProcessingInternal()
{
    int32_t ret = 0;
    if (mAcrylicHandle == NULL) {
        MPP_LOGE("%s:: mAcrylicHandle is NULL", __func__);
        return -EINVAL;
    }
    if (mPhysicalType == MPP_G2D) {
        if ((ret = setG2DColorConversionInfo()) != NO_ERROR) {
            MPP_LOGE("%s:: fail to setColorConversionInfo ret %d", __func__, ret);
            return ret;
        }
    }

    return ExynosMPP::doPostProcessingInternal();
}
#endif
