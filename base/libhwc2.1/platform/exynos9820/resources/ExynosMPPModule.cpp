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

ExynosMPPModule::ExynosMPPModule(uint32_t physicalType, uint32_t logicalType, const char *name,
        uint32_t physicalIndex, uint32_t logicalIndex,
        uint32_t preAssignInfo, uint32_t mppType)
    : ExynosMPP(physicalType, logicalType, name, physicalIndex, logicalIndex, preAssignInfo, mppType)
{
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
    if (src.compressionInfo.type == COMP_TYPE_AFBC) {
        if (mPhysicalType == MPP_G2D)
            return true;
        if ((mPhysicalType != MPP_DPP_GF) && (mPhysicalType != MPP_DPP_VGF) &&
            (mPhysicalType != MPP_DPP_VGRFS))
            return false;

        if (mSharedMPP == NULL) {
            MPP_LOGE("sharedMPP is NULL");
            return false;
        }

        if ((mSharedMPP->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED) == 0)
            return true;
        else {
            if (mSharedMPP->mAssignedSources.size() == 1) {
                exynos_image checkImg = mSharedMPP->mAssignedSources[0]->mSrcImg;
                if ((mSharedMPP->mAssignedSources[0]->mSourceType == MPP_SOURCE_COMPOSITION_TARGET) ||
                    (mSharedMPP->mAssignedSources[0]->mM2mMPP != NULL)) {
                    checkImg = mSharedMPP->mAssignedSources[0]->mMidImg;
                }
                if (checkImg.transform & HAL_TRANSFORM_ROT_90)
                    return false;

                if (checkImg.compressionInfo.type == COMP_TYPE_NONE)
                    return true;

                if (checkImg.w > 2048)
                    return false;
                else {
                    if (src.w < 2048)
                        return true;
                    else
                        return false;
                }
            } else {
                MPP_LOGE("Invalid mSharedMPP[%d, %d] mAssignedSources size(%zu)",
                        mSharedMPP->mPhysicalType,
                        mSharedMPP->mPhysicalIndex,
                        mSharedMPP->mAssignedSources.size());
            }
        }
        return false;
    } else {
        return true;
    }
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
        if ((src.transform & HAL_TRANSFORM_ROT_90) == 0)
        {
            if ((src.compressionInfo.type == COMP_TYPE_AFBC) &&
                    (src.transform != 0))
                return false;
            return true;
        } else {
            return false;
        }
    case MPP_DPP_VGRFS:
        if (src.exynosFormat.isYUV420()) {
            /* HACK */
            if ((src.transform != 0) && src.exynosFormat.is10BitYUV420()) return false;

            if (mSharedMPP == NULL) {
                MPP_LOGE("sharedMPP is NULL");
                return false;
            }

            if ((src.transform & HAL_TRANSFORM_ROT_90) &&
                (mSharedMPP->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED)) {
                if (mSharedMPP->mAssignedSources.size() != 1) {
                    MPP_LOGE("Invalid sharedMPP[%d, %d] mAssignedSources size(%zu)",
                            mSharedMPP->mPhysicalType,
                            mSharedMPP->mPhysicalIndex,
                            mSharedMPP->mAssignedSources.size());
                    return false;
                }
                exynos_image checkImg = mSharedMPP->mAssignedSources[0]->mSrcImg;
                if ((mSharedMPP->mAssignedSources[0]->mSourceType == MPP_SOURCE_COMPOSITION_TARGET) ||
                    (mSharedMPP->mAssignedSources[0]->mM2mMPP != NULL)) {
                    checkImg = mSharedMPP->mAssignedSources[0]->mMidImg;
                }
                if (checkImg.compressionInfo.type == COMP_TYPE_AFBC)
                    return false;
                }
            return true;
        }
        /* RGB case */
        if ((src.transform & HAL_TRANSFORM_ROT_90) == 0)
        {
            if ((src.compressionInfo.type == COMP_TYPE_AFBC) &&
                    (src.transform != 0))
                return false;
            return true;
        } else {
            return false;
        }
    default:
            return true;
    }
}

uint32_t ExynosMPPModule::getSrcMaxCropSize(struct exynos_image &src)
{
    if ((mPhysicalType == MPP_DPP_VGRFS) &&
        (src.transform & HAL_TRANSFORM_ROT_90))
        return MAX_DPP_ROT_SRC_SIZE;
    else
        return ExynosMPP::getSrcMaxCropSize(src);
}

bool ExynosMPPModule::isSrcFormatSupported(struct exynos_image &src)
{
    if ((mPhysicalType == MPP_MSC) &&
        src.exynosFormat.isRgb()) {
        return false;
    }

    return ExynosMPP::isSrcFormatSupported(src);
}

bool ExynosMPPModule::isDstFormatSupported(struct exynos_image &dst)
{

    if ((mPhysicalType == MPP_MSC) &&
        dst.exynosFormat.isRgb()) {
        return false;
    }

    return ExynosMPP::isDstFormatSupported(dst);
}

