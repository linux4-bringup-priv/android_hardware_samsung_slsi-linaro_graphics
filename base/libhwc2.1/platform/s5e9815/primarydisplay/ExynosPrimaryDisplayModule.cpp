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
#include "ExynosLayer.h"

ExynosPrimaryDisplayModule::ExynosPrimaryDisplayModule(DisplayIdentifier node)
    :    ExynosPrimaryDisplay(node)
{
}

ExynosPrimaryDisplayModule::~ExynosPrimaryDisplayModule () {
}

int32_t ExynosPrimaryDisplayModule::validateWinConfigData()
{
    if (mDisplayConfigPending)
        setActiveConfigInternal(mActiveConfig);

    return ExynosDisplay::validateWinConfigData();
}

int32_t ExynosPrimaryDisplayModule::configureOverlay(ExynosCompositionInfo &compositionInfo) {
    int32_t ret = ExynosDisplay::configureOverlay(compositionInfo);
    int32_t windowIndex = compositionInfo.mWindowIndex;

    if ((windowIndex < 0) || (windowIndex >= (int32_t)mDpuData.configs.size())) {
        DISPLAY_LOGE("%s:: ExynosCompositionInfo(%d) has invalid data, windowIndex(%d)",
                __func__, compositionInfo.mType, windowIndex);
        return -EINVAL;
    }
    exynos_win_config_data &config = mDpuData.configs[windowIndex];

    if (config.fd_idma[2] == -1) {
        int hdrIndex = -1;

        for (size_t i = (size_t)compositionInfo.mFirstIndex; i <= (size_t)compositionInfo.mLastIndex; i++) {
            ExynosLayer *layer = mLayers[i];
            if (layer->mIsHdrLayer == true) {
                if (layer->mExynosCompositionType == HWC2_COMPOSITION_DEVICE) {
                    continue;
                } else {
                    config.hdrLayerDataspace = layer->mDataSpace;
                    hdrIndex = i;
                }
            }
        }
        if (hdrIndex >= 0) {
            /* When GPU target buffer includes HDR layer,
               HDR metadata is set the fd2 of GPU target buffer */
            if (mLayers[hdrIndex]->mMetaParcelFd >= 0)
                config.fd_idma[2] = mLayers[hdrIndex]->mMetaParcelFd;
        }
    }
    return ret;
}

void ExynosPrimaryDisplayModule::resetForDestroyClient() {
    if (!mPlugState)
        return;

    mConfigRequestState = hwc_request_state_t::SET_CONFIG_STATE_NONE;
    mDesiredConfig = 0;
    uint64_t temp = 0;
    setPowerMode(HWC2_POWER_MODE_ON, temp);
    mActiveConfig = mDisplayInterface->getPreferredModeId();
    setActiveConfigInternal(mActiveConfig);
    setVsyncEnabledInternal(HWC2_VSYNC_DISABLE);
    mVsyncCallback.resetVsyncTimeStamp();
    mVsyncCallback.resetDesiredVsyncPeriod();
    mVsyncCallback.enableVSync(0);
}
