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
#define MPP_G2D_HDR_PIPE_NUM 4

class ExynosMPPModule : public ExynosMPP {
    public:
        ExynosMPPModule(uint32_t physicalType, uint32_t logicalType, const char *name,
            uint32_t physicalIndex, uint32_t logicalIndex, uint32_t preAssignInfo, uint32_t mppType);
        ~ExynosMPPModule();
        virtual bool isDataspaceSupportedByMPP(struct exynos_image &src, struct exynos_image &dst);
    protected:
        virtual uint32_t getMPPClock();

    public:
#ifdef USE_HDR_INTERFACE
        int mHdrMap[MPP_G2D_HDR_PIPE_NUM];
        Vector<android_dataspace> mAssignedDataspace;
        Vector<int> mAssignedLuminance;
        hdrInterface *mHdrCoefInterface;
        bool isQualifiedHDRPipeRestriction(DisplayInfo &display,
                struct exynos_image &src, struct exynos_image &dst);
        virtual int32_t resetMPP();
        virtual int32_t resetAssignedState();
        virtual int32_t resetAssignedState(ExynosMPPSource *mppSource);
        virtual bool isAssignableState(DisplayInfo &display, struct exynos_image &src, struct exynos_image &dst);
        int32_t setG2DColorConversionInfo();
        virtual int32_t assignMPP(DisplayInfo &display, ExynosMPPSource *mppSource);
        virtual int32_t doPostProcessingInternal();
#endif
};

#endif
