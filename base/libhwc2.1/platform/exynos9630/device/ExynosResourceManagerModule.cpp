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
#include "ExynosResourceManagerModule.h"
#include "ExynosPrimaryDisplayModule.h"
#include "ExynosMPPModule.h"
#include "ExynosLayer.h"
#include "ExynosHWCHelper.h"
#include "ExynosGraphicBuffer.h"

using namespace vendor::graphics;

ExynosResourceManagerModule::ExynosResourceManagerModule()
        : ExynosResourceManager()
{
}

ExynosResourceManagerModule::~ExynosResourceManagerModule()
{
}

int32_t ExynosResourceManagerModule::prepareResources()
{
    int32_t ret = ExynosResourceManager::prepareResources();
    for (size_t i = 0; i < mDisplays.size(); i++) {
        if (mDisplays[i]->mPlugState == false)
            continue;
        for (size_t j = 0; j < mM2mMPPs.size(); j++) {
            if ((mM2mMPPs[j]->mPhysicalType == MPP_G2D) &&
                (mM2mMPPs[j]->mReservedDisplayInfo.displayIdentifier.id ==
                 mDisplays[i]->mDisplayId))
                ((ExynosMPPModule*)mM2mMPPs[j])->mHdrCoefInterface = mDisplays[i]->mHdrCoefInterface;
        }
    }
    return ret;
}

void ExynosResourceManagerModule::preAssignWindows()
{
    ExynosPrimaryDisplayModule *primaryDisplay =
        (ExynosPrimaryDisplayModule *)getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    primaryDisplay->mBaseWindowIndex = 0;
    primaryDisplay->mMaxWindowNum =
        primaryDisplay->mDisplayInterface->getMaxWindowNum();

    bool externalConnected = false;
    for (auto display: mDisplays) {
        if ((display == nullptr) ||
                (display->mType == HWC_DISPLAY_PRIMARY) ||
                (display->mUseDpu == false))
            continue;
        if (display->mPlugState == true) {
            if (display->mType == HWC_DISPLAY_EXTERNAL)
                externalConnected = true;
        }
    }

    if (externalConnected) {
        primaryDisplay->mBaseWindowIndex = PRIMARY_DISP_BASE_WIN[mDeviceInfo.displayMode];
        primaryDisplay->mMaxWindowNum -= PRIMARY_DISP_BASE_WIN[mDeviceInfo.displayMode];
    }
}
