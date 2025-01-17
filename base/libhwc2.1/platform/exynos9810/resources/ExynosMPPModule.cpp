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

bool ExynosMPPModule::checkCSCRestriction(struct exynos_image &src, struct exynos_image &dst)
{
    if (mPhysicalType == MPP_DPP_VGRFS) {
        /* VGRFS only supports 10bit YUV HDR*/
        if (src.exynosFormat.is10BitYUV420() == false)
            return false;
    }
    return true;
}

bool ExynosMPPModule::checkRotationCondition(struct exynos_image &src) {
    if (mPhysicalType == MPP_DPP_VGRFS) {
        if (hasHdrInfo(src) && (src.transform & HAL_TRANSFORM_ROT_90))
            return false;
    }
    return ExynosMPP::checkRotationCondition(src);
}

uint32_t ExynosMPPModule::getSrcXOffsetAlign(struct exynos_image &src) {
    /* Refer module(ExynosMPPModule) for chip specific restrictions */
    uint32_t idx = getRestrictionClassification(src);
    if ((mPhysicalType == MPP_MSC) &&
        ((src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B) ||
         (src.exynosFormat == HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B))) {
        return 16;
    }
    return mSrcSizeRestrictions[idx].cropXAlign;
}
