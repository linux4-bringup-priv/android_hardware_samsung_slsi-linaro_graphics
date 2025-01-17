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
#include "ExynosVirtualDisplay.h"
#include "ExynosGraphicBuffer.h"

ExynosResourceManagerModule::ExynosResourceManagerModule()
        : ExynosResourceManager()
{
    mLayerAttributePriority.resize(sizeof(product_layerAttributePriority)/sizeof(product_layerAttributePriority[0]));

    for (uint32_t i = 0; i < sizeof(product_layerAttributePriority)/sizeof(product_layerAttributePriority[0]); i++)
        mLayerAttributePriority.add(product_layerAttributePriority[i]);

    for (auto mpp: mOtfMPPs) {
        ExynosMPPModule *cur = (ExynosMPPModule*)mpp;
        cur->mVirtual8KMPP = cur->mSubMPP[0] = cur->mSubMPP[1] = NULL;
    }

    for (size_t i = 0; i < (sizeof(VIRTUAL_CHANNEL_PAIR_MAP) / sizeof(virtual_dpp_map)); i++) {
        virtual_dpp_map_t map = VIRTUAL_CHANNEL_PAIR_MAP[i];
        for (auto otfMPP: mOtfMPPs) {
            if ((map.logicalType == otfMPP->mLogicalType) &&
                (map.physicalIndex == otfMPP->mPhysicalIndex) &&
                (map.physicalType == otfMPP->mPhysicalType)) {
                ExynosMPPModule *cur = (ExynosMPPModule*)otfMPP;
                if (cur) {
                    cur->mSubMPP[0] = getExynosMPP(map.physicalType, map.physicalIndex1);
                    cur->mSubMPP[1] = getExynosMPP(map.physicalType, map.physicalIndex2);
                    ((ExynosMPPModule *)(cur->mSubMPP[0]))->mVirtual8KMPP = otfMPP;
                    ((ExynosMPPModule *)(cur->mSubMPP[1]))->mVirtual8KMPP = otfMPP;
                }
            }
        }
    }
}

ExynosResourceManagerModule::~ExynosResourceManagerModule()
{
}

void ExynosResourceManagerModule::setFrameRateForPerformance(ExynosMPP &mpp,
        AcrylicPerformanceRequestFrame *frame)
{
    ExynosResourceManager::setFrameRateForPerformance(mpp, frame);

    // Required processing time of LLWFD is 14ms
    // 14ms -> 71Hz
    if (mpp.mLogicalType == MPP_LOGICAL_G2D_COMBO &&
            mpp.mAssignedDisplayInfo.displayIdentifier.id != UINT_MAX) {
        DisplayInfo &display = mpp.mAssignedDisplayInfo;
        if ((display.displayIdentifier.type == HWC_DISPLAY_VIRTUAL) &&
            (display.isWFDState == LLWFD))
            frame->setFrameRate(71);
    }
}

uint32_t ExynosResourceManagerModule::getExceptionScenarioFlag(ExynosMPP *mpp) {

    uint32_t ret = ExynosResourceManager::getExceptionScenarioFlag(mpp);

    if (!((mpp->mPhysicalType == MPP_DPP_VGS) && (mpp->mPhysicalIndex == 1)))
        return ret;

    bool WFDConnected = false;
    /* WFD connection check */
    for (size_t j = 0; j < mDisplays.size(); j++) {
        ExynosDisplay* display = mDisplays[j];
        if (display->mType == HWC_DISPLAY_VIRTUAL) {
            if (((ExynosVirtualDisplay *)display)->mPlugState) {
                WFDConnected = true;
                break;
            }
        } else continue;
    }

    /* VGS are used by libWB, So primary/external display are can't use VGS */
    if (!(WFDConnected) || !(mpp->mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT))
        ret |= static_cast<uint32_t>(DisableType::DISABLE_SCENARIO);

    return ret;
}

void ExynosResourceManagerModule::setVirtualOtfMPPsRestrictions()
{
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        // mAttr should be updated with updated feature_table
        if ((mOtfMPPs[i]->mLogicalType == MPP_LOGICAL_DPP_VGS8K) ||
                (mOtfMPPs[i]->mLogicalType == MPP_LOGICAL_DPP_VGFS8K) ||
                (mOtfMPPs[i]->mLogicalType == MPP_LOGICAL_DPP_VGRFS8K))
        {
            mOtfMPPs[i]->mAttr &= (~MPP_ATTR_DIM) & (~MPP_ATTR_WINDOW_UPDATE) & (~MPP_ATTR_BLOCK_MODE);
            for (uint32_t j = 0; j < RESTRICTION_MAX; j++) {
                /* Src min/max size */
                mOtfMPPs[i]->mSrcSizeRestrictions[j].minCropWidth = mOtfMPPs[i]->mSrcSizeRestrictions[j].maxCropWidth;
                mOtfMPPs[i]->mSrcSizeRestrictions[j].minCropHeight = mOtfMPPs[i]->mSrcSizeRestrictions[j].maxCropHeight;
                mOtfMPPs[i]->mSrcSizeRestrictions[j].maxCropWidth = VIRTUAL_8K_WIDTH;
            }
        }
    }

    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        for (uint32_t j = 0; j < RESTRICTION_MAX; j++) {
            ExynosMPP *mpp = mOtfMPPs[i];
            HDEBUGLOGD(eDebugMPP, "\tModified mSrcSizeRestrictions[%d], "
                    "[%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d]",
                    i, mpp->mSrcSizeRestrictions[j].maxDownScale, mpp->mSrcSizeRestrictions[j].maxUpScale,
                    mpp->mSrcSizeRestrictions[j].maxFullWidth, mpp->mSrcSizeRestrictions[j].maxFullHeight,
                    mpp->mSrcSizeRestrictions[j].minFullWidth, mpp->mSrcSizeRestrictions[j].minFullHeight,
                    mpp->mSrcSizeRestrictions[j].fullWidthAlign, mpp->mSrcSizeRestrictions[j].fullHeightAlign,
                    mpp->mSrcSizeRestrictions[j].maxCropWidth, mpp->mSrcSizeRestrictions[j].maxCropHeight,
                    mpp->mSrcSizeRestrictions[j].minCropWidth, mpp->mSrcSizeRestrictions[j].minCropHeight,
                    mpp->mSrcSizeRestrictions[j].cropXAlign, mpp->mSrcSizeRestrictions[j].cropYAlign,
                    mpp->mSrcSizeRestrictions[j].cropWidthAlign, mpp->mSrcSizeRestrictions[j].cropHeightAlign);
            HDEBUGLOGD(eDebugMPP, "\ttModified mDstSizeRestrictions[%d], "
                    "[%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d]",
                    i, mpp->mDstSizeRestrictions[j].maxDownScale, mpp->mDstSizeRestrictions[j].maxUpScale,
                    mpp->mDstSizeRestrictions[j].maxFullWidth, mpp->mDstSizeRestrictions[j].maxFullHeight,
                    mpp->mDstSizeRestrictions[j].minFullWidth, mpp->mDstSizeRestrictions[j].minFullHeight,
                    mpp->mDstSizeRestrictions[j].fullWidthAlign, mpp->mDstSizeRestrictions[j].fullHeightAlign,
                    mpp->mDstSizeRestrictions[j].maxCropWidth, mpp->mDstSizeRestrictions[j].maxCropHeight,
                    mpp->mDstSizeRestrictions[j].minCropWidth, mpp->mDstSizeRestrictions[j].minCropHeight,
                    mpp->mDstSizeRestrictions[j].cropXAlign, mpp->mDstSizeRestrictions[j].cropYAlign,
                    mpp->mDstSizeRestrictions[j].cropWidthAlign, mpp->mDstSizeRestrictions[j].cropHeightAlign);
        }
    }

    return;
}

void ExynosResourceManagerModule::preAssignWindows()
{
    ExynosPrimaryDisplayModule *primaryDisplay =
        (ExynosPrimaryDisplayModule *)getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    primaryDisplay->mBaseWindowIndex = 0;
    primaryDisplay->mMaxWindowNum =
        primaryDisplay->mDisplayInterface->getMaxWindowNum();

    bool virtualConnected = false;
    bool externalConnected = false;
    for (auto display: mDisplays) {
        if ((display == nullptr) ||
            (display->mType == HWC_DISPLAY_PRIMARY) ||
            (display->mUseDpu == false))
            continue;
        if (display->mPlugState == true) {
            if (display->mType == HWC_DISPLAY_VIRTUAL)
                virtualConnected = true;
            else if (display->mType == HWC_DISPLAY_EXTERNAL)
                externalConnected = true;
        }
    }
    if (virtualConnected) {
        primaryDisplay->mBaseWindowIndex += PRIMARY_MAIN_VIRTUAL_WINCNT;
        primaryDisplay->mMaxWindowNum -= PRIMARY_MAIN_VIRTUAL_WINCNT;
    } else if (externalConnected) {
        primaryDisplay->mBaseWindowIndex +=
            EXTERNAL_WINDOW_COUNT[mDeviceInfo.displayMode];
        primaryDisplay->mMaxWindowNum -= EXTERNAL_WINDOW_COUNT[mDeviceInfo.displayMode];
    }
}
