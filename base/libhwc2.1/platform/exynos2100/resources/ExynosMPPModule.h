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
#ifndef _EXYNOS_MPP_MODULE_H
#define _EXYNOS_MPP_MODULE_H

#include "ExynosMPP.h"

#define MAX_DPP_ROT_SRC_SIZE (3840*2160)
#define MAX_DPP_8K_SRC_SIZE  (7680*4320)

static constexpr uint32_t VGFS_AFBC_WIDTH_LIMIT = 2048;
class ExynosMPPModule : public ExynosMPP {
    public:
        ExynosMPPModule(uint32_t physicalType, uint32_t logicalType, const char *name,
            uint32_t physicalIndex, uint32_t logicalIndex, uint32_t preAssignInfo, uint32_t mppType);
        ~ExynosMPPModule();
        virtual bool getSubMPPs(ExynosMPP** subMPP0, ExynosMPP** subMPP1) override {
            if ((mSubMPP[0] == nullptr) || (mSubMPP[1] == nullptr))
                return false;
            *subMPP0 = mSubMPP[0];
            *subMPP1 = mSubMPP[1];
            return true;
        };

        virtual bool isSupportedTransform(struct exynos_image &src);
        virtual bool isSupportedCompression(struct exynos_image &src);
        virtual uint32_t getDstWidthAlign(struct exynos_image &dst);
        virtual uint32_t getSrcMaxCropSize(struct exynos_image &src);
        virtual uint32_t getSrcMaxCropWidth(struct exynos_image &src);
        virtual uint32_t getSrcMaxCropHeight(struct exynos_image &src);
        virtual bool hasEnoughCapa(DisplayInfo &display, struct exynos_image &src,
                struct exynos_image &dst, float totalUsedCapa) override;
        virtual bool isSupportedCapability(DisplayInfo &display,
                struct exynos_image &src) override;
        virtual bool isSharedMPPUsed();
        virtual int32_t setVotfLayerData(exynos_mpp_img_info *srcImgInfo);
        virtual bool canUseVotf(struct exynos_image &src) override;
        virtual uint32_t getMaxDownscale(DisplayInfo &display,
                struct exynos_image &src, struct exynos_image &dst) override;
        virtual bool isCapacityExceptionCondition(float totalUsedCapacity, float requiredCapacity, struct exynos_image &src);
        virtual bool scaleAllowedByDPPPerformance(DisplayInfo &display,
                struct exynos_image &src, struct exynos_image &dst) override;
    public:
        ExynosMPP *mSubMPP[2]; //This MPP points sub physical MPP if MPP is virtual8K otf
        ExynosMPP *mVirtual8KMPP = NULL; //This MPP points virtual 8K otf MPP.
};

#endif
