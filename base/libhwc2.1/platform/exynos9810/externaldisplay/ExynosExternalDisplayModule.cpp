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
//#define LOG_NDEBUG 0
#include "ExynosExternalDisplayModule.h"
#include "ExynosPrimaryDisplayModule.h"

#ifdef USES_VIRTUAL_DISPLAY
#include "ExynosVirtualDisplayModule.h"
#endif

#include <linux/fb.h>
#include <android/sync.h>
#include "ExynosLayer.h"
#include "ExynosHWCDebug.h"
#include "ExynosHWCHelper.h"

#define SKIP_FRAME_COUNT        3

ExynosExternalDisplayModule::ExynosExternalDisplayModule(DisplayIdentifier node)
    :    ExynosExternalDisplay(node)
{
    mBlanked = true;
}

ExynosExternalDisplayModule::~ExynosExternalDisplayModule ()
{

}

int32_t ExynosExternalDisplayModule::validateWinConfigData()
{
    bool flagValidConfig = true;

    if (ExynosDisplay::validateWinConfigData() != NO_ERROR)
        flagValidConfig = false;

    for (size_t i = 0; i < mDpuData.configs.size(); i++) {
        struct exynos_win_config_data &config = mDpuData.configs[i];
        if (config.state == config.WIN_STATE_BUFFER) {
            bool configInvalid = false;
            uint32_t mppType = config.assignedMPP->mPhysicalType;
            if ((config.src.w != config.dst.w) ||
                (config.src.h != config.dst.h)) {
                if ((mppType == MPP_DPP_G) ||
                    (mppType == MPP_DPP_VG)) {
                    DISPLAY_LOGE("WIN_CONFIG error: invalid assign id : %zu,  s_w : %d, d_w : %d, s_h : %d, d_h : %d, mppType : %d",
                            i, config.src.w, config.dst.w, config.src.h, config.dst.h, mppType);
                    configInvalid = true;
                }
            }
            if (configInvalid) {
                config.state = config.WIN_STATE_DISABLED;
                flagValidConfig = false;
            }
        }
    }
    if (flagValidConfig)
        return NO_ERROR;
    else
        return -EINVAL;
}

int32_t ExynosExternalDisplayModule::preProcessValidate(
        DeviceValidateInfo &validateInfo, uint64_t &geometryChanged)
{
    {
        Mutex::Autolock lock(mExternalMutex);
        DISPLAY_LOGD(eDebugExternalDisplay, "");
        if ((mSkipStartFrame > (SKIP_EXTERNAL_FRAME - 1)) && (mEnabled == false) &&
                (mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_NORMAL)) {
            if (enableDecon()) {
                mRenderingStateFlags.validateFlag = true;
                return HWC_HAL_ERROR_INVAL;
            }
        }
    }
    return ExynosExternalDisplay::preProcessValidate(validateInfo, geometryChanged);
}


int ExynosExternalDisplayModule::enable()
{
    ALOGI("[ExternalDisplay] %s +", __func__);

    if (mEnabled)
        return HWC2_ERROR_NONE;

    if (mHpdStatus == false) {
        ALOGI("HPD is not connected");
        return HWC2_ERROR_NONE;
    }

    if (mDisplayInterface->setPowerMode(HWC_POWER_MODE_NORMAL) < 0){
        DISPLAY_LOGE("set powermode ioctl failed errno : %d", errno);
        return HWC2_ERROR_UNSUPPORTED;
    }

    clearDisplay();

    mEnabled = true;

    ALOGI("[ExternalDisplay] %s -", __func__);

    return HWC2_ERROR_NONE;
}

int32_t ExynosExternalDisplayModule::enableDecon()
{
    int fb_blank = 0;
    int err = 0;

    fb_blank = FB_BLANK_UNBLANK;
    err = enable();
    if (err != 0) {
        DISPLAY_LOGE("set powermode ioctl failed errno : %d", errno);
        return HWC2_ERROR_UNSUPPORTED;
    }

    DISPLAY_LOGD(eDebugExternalDisplay, "%s:: blank(%d)", __func__, fb_blank);

    return HWC2_ERROR_NONE;
}

int32_t ExynosExternalDisplayModule::setPowerMode(int32_t mode,
        uint64_t &geometryFlag) {
    Mutex::Autolock lock(mExternalMutex);
    DISPLAY_LOGD(eDebugExternalDisplay, "%s:: mode(%d)", __func__, mode);

    if (mode == HWC_POWER_MODE_OFF) {
        clearDisplay();
        mBlanked = true;
        /* It should be called from validate() when the screen is on */
        mNeedSkipPresent = true;
        setGeometryChanged(GEOMETRY_DISPLAY_POWER_OFF, geometryFlag);
        if ((mRenderingState >= RENDERING_STATE_VALIDATED) &&
            (mRenderingState < RENDERING_STATE_PRESENTED))
            closeFencesForSkipFrame(RENDERING_STATE_VALIDATED);
        mRenderingState = RENDERING_STATE_NONE;
    } else {
        mBlanked = false;
    }
    return HWC2_ERROR_NONE;
}

void ExynosExternalDisplayModule::handleHotplugEvent(bool hpdStatus) {
    Mutex::Autolock lock(mExternalMutex);
    {
        Mutex::Autolock lock(mDisplayMutex);
        mHpdStatus = hpdStatus;
        if (mHpdStatus) {
            if (openExternalDisplay() < 0) {
                DISPLAY_LOGE("Failed to openExternalDisplay");
                mHpdStatus = false;
                return;
            }
            mPowerModeState = (hwc2_power_mode_t)HWC_POWER_MODE_NORMAL;
        }
        else {
            disable();
            closeExternalDisplay();
        }
    }

    ALOGI("HPD status changed to %s, mDisplayId %d, mDisplayFd %d", mHpdStatus ? "enabled" : "disabled", mDisplayId, mDisplayInterface->getDisplayFd());
}

int32_t ExynosExternalDisplayModule::getHdrCapabilities(uint32_t* outNumTypes, int32_t* outTypes, float* __unused outMaxLuminance,
        float* __unused outMaxAverageLuminance, float* __unused outMinLuminance)
{
    DISPLAY_LOGD(eDebugExternalDisplay, "HWC2: %s, %d", __func__, __LINE__);

    if (outTypes == NULL) {
        *outNumTypes = 1;
        return HWC2_ERROR_NONE;
    }

    outTypes[0] = HAL_HDR_HDR10;

    return HWC2_ERROR_NONE;
}

void ExynosExternalDisplayModule::closeExternalDisplay()
{
    ExynosExternalDisplay::closeExternalDisplay();
    mBlanked = true;
}
