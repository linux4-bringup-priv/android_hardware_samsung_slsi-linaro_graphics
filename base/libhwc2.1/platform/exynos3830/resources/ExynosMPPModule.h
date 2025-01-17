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

#ifndef MSC_CLOCK
#define MSC_CLOCK   667000
#endif

#include "ExynosMPP.h"

#define MAX_DPP_ROT_SRC_SIZE (3040*1440)

class ExynosMPPModule : public ExynosMPP {
    public:
        ExynosMPPModule(uint32_t physicalType, uint32_t logicalType, const char *name,
            uint32_t physicalIndex, uint32_t logicalIndex, uint32_t preAssignInfo, uint32_t mppType);
        ~ExynosMPPModule();
        virtual uint32_t getSrcXOffsetAlign(struct exynos_image &src);
        virtual bool isSrcFormatSupported(struct exynos_image &src);
        virtual uint32_t getSrcMaxBlendingNum(struct exynos_image &src, struct exynos_image &dst);
        virtual bool hasEnoughCapa(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst, float totalUsedCapa) override;
};

#endif
