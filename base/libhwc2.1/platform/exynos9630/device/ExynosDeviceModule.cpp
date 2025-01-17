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

#include "ExynosDisplay.h"
#include "ExynosLayer.h"
#include "ExynosDeviceModule.h"
#include "ExynosGraphicBuffer.h"

using vendor::graphics::ExynosGraphicBufferMeta;

ExynosDeviceModule::ExynosDeviceModule()
{
}

ExynosDeviceModule::~ExynosDeviceModule() {
}

bool ExynosDeviceModule::getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *minClock) {
    ExynosDisplay *_display = mDisplays[display];
    if (_display == NULL) return false;

    if (config == SF_PERF_MODE_SET)
        *cpuIDs = CPU_CLUSTER1_MASK;
    else if (config == SF_PERF_MODE_RESET)
        *cpuIDs = CPU_CLUSTER0_MASK | CPU_CLUSTER1_MASK;
    else {
        ALOGI("%s:: config is invalid : [display %d] %d", __func__, display, config);
        return false;
    }

    *minClock = 0;

    return true;
}

int32_t ExynosDeviceModule::presentDisplay(ExynosDisplay *display,
        int32_t *outPresentFence)
{
#if defined(EPIC_LIBRARY_PATH) && !defined(BUILD_IN_UNIVERSAL)
    constexpr uint32_t standard_layer_nums = 3;
    if ((mEPICAcquireConditional && mEPICFreeConditional) &&
            (display->mType == HWC_DISPLAY_PRIMARY)) {
        bool isUsingEPIC = false;
        bool ret = true;

        if (mBoostingEPICHandle == 0 && mEPICRequestFcnPtr)
            mBoostingEPICHandle = mEPICRequestFcnPtr(BOOST_SCENARIO_NUM);

        if ((display->mLayers.size() >= standard_layer_nums) &&
                display->mLayers[0]->mLayerBuffer &&
                isFormatYUV(ExynosGraphicBufferMeta::get_format(display->mLayers[0]->mLayerBuffer))) {
            for (uint32_t i = 1; i < standard_layer_nums; i++) {
                ExynosLayer* layer = display->mLayers[i];
                if (WIDTH(layer->mPreprocessedInfo.displayFrame) > (display->mXres * 9 / 10) &&
                    HEIGHT(layer->mPreprocessedInfo.displayFrame) > (display->mYres * 9 / 10) &&
                    (layer->mTransform != 0) &&
                    (layer->mLayerBuffer && (!isAFBCCompressed(layer->mLayerBuffer)))) {
                    isUsingEPIC = true;
                    break;
                }
            }
        }

        if (isUsingEPIC && !mEPICState && mBoostingEPICHandle) {
            ret = mEPICAcquireConditional(mBoostingEPICHandle, BOOST_SCENARIO_NAME, strlen(BOOST_SCENARIO_NAME));
            mEPICState = true;
        } else if (!isUsingEPIC && mEPICState && mBoostingEPICHandle) {
            ret = mEPICFreeConditional(mBoostingEPICHandle, BOOST_SCENARIO_NAME, strlen(BOOST_SCENARIO_NAME));
            mEPICState = false;
        }

        if (ret == false)
            ALOGE("%s:: EPIC function call has problem", __func__);
    }
#endif
    return ExynosDevice::presentDisplay(display, outPresentFence);
}
