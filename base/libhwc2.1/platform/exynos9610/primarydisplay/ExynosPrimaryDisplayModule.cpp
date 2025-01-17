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

#include "ExynosPrimaryDisplayModule.h"
#include "ExynosHWCDebug.h"

ExynosPrimaryDisplayModule::ExynosPrimaryDisplayModule(DisplayIdentifier node)
    :    ExynosPrimaryDisplay(node)
{
}

ExynosPrimaryDisplayModule::~ExynosPrimaryDisplayModule () {
}

int32_t ExynosPrimaryDisplayModule::getHdrCapabilities(uint32_t *outNumTypes,
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

    uint32_t vectorSize = static_cast<uint32_t>(mHdrTypes.size());
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
