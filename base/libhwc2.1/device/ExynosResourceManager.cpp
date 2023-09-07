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

/**
 * Project HWC 2.0 Design
 */

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
#include <cutils/properties.h>
#include <unordered_set>
#include "ExynosResourceManager.h"
#include "ExynosMPPModule.h"
#include "ExynosLayer.h"
#include "ExynosHWCDebug.h"
#include "hardware/exynos/acryl.h"
#include "ExynosPrimaryDisplayModule.h"
#include "ExynosVirtualDisplay.h"
#include "ExynosExternalDisplay.h"
#include "ExynosDeviceInterface.h"

#ifndef USE_MODULE_ATTR
/* Basic supported features */
feature_support_t feature_table[] =
    {
        {MPP_DPP_G,
         MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM},

        {MPP_DPP_GF,
         MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM},

        {MPP_DPP_VG,
         MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM},

        {MPP_DPP_VGS,
         MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE | MPP_ATTR_DIM},

        {MPP_DPP_VGF,
         MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM},

        {MPP_DPP_VGFS,
         MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE | MPP_ATTR_DIM},

        {MPP_DPP_VGRFS,
         MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE |
             MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90 |
             MPP_ATTR_DIM | MPP_ATTR_HDR10},

        {MPP_MSC,
         MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90},

        {MPP_G2D,
         MPP_ATTR_AFBC | MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90 |
             MPP_ATTR_HDR10 | MPP_ATTR_USE_CAPA}};
#endif

using namespace android;

ExynosMPPVector ExynosResourceManager::mOtfMPPs;
ExynosMPPVector ExynosResourceManager::mM2mMPPs;
std::vector<EnableMPPRequest> ExynosResourceManager::mEnableMPPRequests;

extern struct exynos_hwc_control exynosHWCControl;

ExynosMPPVector::ExynosMPPVector() {
}

ExynosMPPVector::ExynosMPPVector(const ExynosMPPVector &rhs)
    : android::SortedVector<ExynosMPP *>(rhs) {
}

int ExynosMPPVector::do_compare(const void *lhs, const void *rhs) const {
    if (lhs == NULL || rhs == NULL)
        return 0;

    const ExynosMPP *l = *((ExynosMPP **)(lhs));
    const ExynosMPP *r = *((ExynosMPP **)(rhs));

    if (l == NULL || r == NULL)
        return 0;

    if (l->mPhysicalType != r->mPhysicalType) {
        return l->mPhysicalType - r->mPhysicalType;
    }

    if (l->mLogicalType != r->mLogicalType) {
        return l->mLogicalType - r->mLogicalType;
    }

    if (l->mPhysicalIndex != r->mPhysicalIndex) {
        return l->mPhysicalIndex - r->mPhysicalIndex;
    }

    return l->mLogicalIndex - r->mLogicalIndex;
}

/**
 * ExynosResourceManager implementation
 *
 */

ExynosResourceManager::ExynosResourceManager() {
    memset(mSizeRestrictionCnt, 0, sizeof(mSizeRestrictionCnt));
    memset(mFormatRestrictions, 0, sizeof(mFormatRestrictions));
    memset(mSizeRestrictions, 0, sizeof(mSizeRestrictions));

    size_t num_mpp_units = sizeof(AVAILABLE_OTF_MPP_UNITS) / sizeof(exynos_mpp_t);
    for (size_t i = 0; i < num_mpp_units; i++) {
        exynos_mpp_t exynos_mpp = AVAILABLE_OTF_MPP_UNITS[i];
        ALOGI("otfMPP type(%d, %d), physical_index(%d), logical_index(%d)",
              exynos_mpp.physicalType, exynos_mpp.logicalType,
              exynos_mpp.physical_index, exynos_mpp.logical_index);
        ExynosMPP *exynosMPP = new ExynosMPPModule(exynos_mpp.physicalType,
                                                   exynos_mpp.logicalType, exynos_mpp.name, exynos_mpp.physical_index,
                                                   exynos_mpp.logical_index, exynos_mpp.pre_assign_info, MPP_TYPE_OTF);
        mOtfMPPs.add(exynosMPP);
        if (exynosMPP->isVirtual8KOtf())
            mVirtualMPPNum++;
    }

    num_mpp_units = sizeof(AVAILABLE_M2M_MPP_UNITS) / sizeof(exynos_mpp_t);
    for (size_t i = 0; i < num_mpp_units; i++) {
        exynos_mpp_t exynos_mpp = AVAILABLE_M2M_MPP_UNITS[i];
        ALOGI("m2mMPP type(%d, %d), physical_index(%d), logical_index(%d)",
              exynos_mpp.physicalType, exynos_mpp.logicalType,
              exynos_mpp.physical_index, exynos_mpp.logical_index);
        ExynosMPP *exynosMPP = new ExynosMPPModule(exynos_mpp.physicalType,
                                                   exynos_mpp.logicalType, exynos_mpp.name, exynos_mpp.physical_index,
                                                   exynos_mpp.logical_index, exynos_mpp.pre_assign_info, MPP_TYPE_M2M);
        mM2mMPPs.add(exynosMPP);
    }

    ALOGI("mOtfMPPs(%zu), mM2mMPPs(%zu)", mOtfMPPs.size(), mM2mMPPs.size());
    if (hwcCheckDebugMessages(eDebugResourceManager)) {
        for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
            HDEBUGLOGD(eDebugResourceManager, "otfMPP[%d]", i);
            String8 dumpMPP;
            mOtfMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            HDEBUGLOGD(eDebugResourceManager, "m2mMPP[%d]", i);
            String8 dumpMPP;
            mM2mMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
    }
}

ExynosResourceManager::~ExynosResourceManager() {
    for (int32_t i = mOtfMPPs.size(); i-- > 0;) {
        ExynosMPP *exynosMPP = mOtfMPPs[i];
        delete exynosMPP;
    }
    mOtfMPPs.clear();
    for (int32_t i = mM2mMPPs.size(); i-- > 0;) {
        ExynosMPP *exynosMPP = mM2mMPPs[i];
        delete exynosMPP;
    }
    mM2mMPPs.clear();
}

void ExynosResourceManager::reloadResourceForHWFC() {
    for (int32_t i = mM2mMPPs.size(); i-- > 0;) {
        ExynosMPP *exynosMPP = mM2mMPPs[i];
        if (exynosMPP->mLogicalType == MPP_LOGICAL_G2D_COMBO &&
            (exynosMPP->mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
            exynosMPP->reloadResourceForHWFC();
            break;
        }
    }
}

void ExynosResourceManager::setTargetDisplayLuminance(uint16_t min, uint16_t max) {
    for (int32_t i = mM2mMPPs.size(); i-- > 0;) {
        ExynosMPP *exynosMPP = mM2mMPPs[i];
        if (exynosMPP->mLogicalType == MPP_LOGICAL_G2D_COMBO &&
            (exynosMPP->mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
            exynosMPP->setTargetDisplayLuminance(min, max);
            break;
        }
    }
}

void ExynosResourceManager::setTargetDisplayDevice(int device) {
    for (int32_t i = mM2mMPPs.size(); i-- > 0;) {
        ExynosMPP *exynosMPP = mM2mMPPs[i];
        if (exynosMPP->mLogicalType == MPP_LOGICAL_G2D_COMBO &&
            (exynosMPP->mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
            exynosMPP->setTargetDisplayDevice(device);
            break;
        }
    }
}

int32_t ExynosResourceManager::doAllocLutParcels(ExynosDisplay *display) {
    hdrInterface *hdrLib = display->mHdrCoefInterface;
    if (hdrLib == nullptr)
        return NO_ERROR;

    int size = hdrLib->getHdrCoefSize(HDR_HW_DPU);
    for (size_t i = 0; i < mOtfMPPs.size(); i++) {
        mOtfMPPs[i]->mHdrCoefSize = size;
        if (size <= 0)
            continue;

        if (allocParcelData(&(mOtfMPPs[i]->mLutParcelFd), size) != NO_ERROR) {
            ALOGE("%s::Fail to allocate Lut Parcel data", __func__);
            return -EINVAL;
        }
        mOtfMPPs[i]->mHdrCoefAddr = (void *)mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, mOtfMPPs[i]->mLutParcelFd, 0);
        if (mOtfMPPs[i]->mHdrCoefAddr == MAP_FAILED) {
            ALOGE("%s::Fail to map Lut Parcel data", __func__);
            mOtfMPPs[i]->mHdrCoefAddr = NULL;
        }
    }
    return NO_ERROR;
}

int32_t ExynosResourceManager::doPreProcessing() {
    int32_t ret = NO_ERROR;
    /* Assign m2mMPP's out buffers */
    ExynosDisplay *display = getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    if (display == NULL)
        return -EINVAL;
    ret = (doAllocDstBufs(display->mXres, display->mYres)) | (doAllocLutParcels(display));
    return ret;
}

int32_t ExynosResourceManager::doAllocDstBufs(uint32_t Xres, uint32_t Yres) {
    ATRACE_CALL();
    int32_t ret = NO_ERROR;
    /* Assign m2mMPP's out buffers */

    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->needPreAllocation(mDeviceInfo.displayMode)) {
            uint32_t width = Xres;
            uint32_t height = Yres;
            mM2mMPPs[i]->mFreeOutBufFlag = false;
            for (uint32_t index = 0; index < NUM_MPP_DST_BUFS(mM2mMPPs[i]->mLogicalType); index++) {
                HDEBUGLOGD(eDebugBuf, "%s allocate dst buffer[%d]%p, x: %d, y: %d",
                           __func__, index, mM2mMPPs[i]->mDstImgs[index].bufferHandle, Xres, Yres);
                uint32_t bufAlign = mM2mMPPs[i]->getOutBufAlign();
                // For using the compressed output in G2D composition,
                // allocated buffer size should be larger than FHD.
                if ((mM2mMPPs[i]->mMaxSrcLayerNum > 1) && (mM2mMPPs[i]->mPhysicalType == MPP_G2D) &&
                    (Xres * Yres < 1920 * 1080)) {
                    width = 1080;
                    height = 1920;
                }
                ret = mM2mMPPs[i]->allocOutBuf(ALIGN_UP(width, bufAlign),
                                               ALIGN_UP(height, bufAlign),
                                               DEFAULT_MPP_DST_FORMAT, 0x0, index);
                if (ret < 0) {
                    HWC_LOGE_NODISP("%s:: fail to allocate dst buffer[%d]",
                                    __func__, index);
                    return ret;
                }
                mM2mMPPs[i]->mPrevAssignedDisplayType = HWC_DISPLAY_PRIMARY;
            }
        }
    }
    return ret;
}

/**
 * @param * display
 * @return int
 */
int32_t ExynosResourceManager::assignResource(ExynosDisplay *display) {
    ATRACE_CALL();
    int ret = 0;

    HDEBUGLOGD(eDebugResourceManager | eDebugSkipResourceAssign,
               "%s::display(%d)", __func__, display->mType);

    HDEBUGLOGD(eDebugTDM, "%s layer's calculation start", __func__);
    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        display->mLayers[i]->resetValidateData();
        calculateHWResourceAmount(display, display->mLayers[i]);
    }

    display->initializeValidateInfos();

    /*
     * Check HDR10+ layers > HDR10+ IPs
     * If ture, HDR10+ layers are changed to HDR10 layers
     */
    if (display->mHasHdr10PlusLayer) {
        processHdr10plusToHdr10(display);
    }

    for (size_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mReservedDisplayInfo.displayIdentifier.id == display->mDisplayId)
            mM2mMPPs[i]->mPreAssignedCapacity = 0.0f;
    }

    if ((ret = updateSupportedMPPFlag(display)) != NO_ERROR) {
        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: updateSupportedMPPFlag() error (%d)",
                 __func__, ret);
        return ret;
    }

    if ((ret = assignResourceInternal(display)) != NO_ERROR) {
        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: assignResourceInternal() error (%d)",
                 __func__, ret);
        return ret;
    }

    if ((ret = assignWindow(display)) != NO_ERROR) {
        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: assignWindow() error (%d)",
                 __func__, ret);
        return ret;
    }

    if (hwcCheckDebugMessages(eDebugResourceManager)) {
        HDEBUGLOGD(eDebugResourceManager, "AssignResource result");
        String8 result;
        display->mClientCompositionInfo.dump(result);
        HDEBUGLOGD(eDebugResourceManager, "%s", result.c_str());
        result.clear();
        display->mExynosCompositionInfo.dump(result);
        HDEBUGLOGD(eDebugResourceManager, "%s", result.c_str());
        for (uint32_t i = 0; i < display->mLayers.size(); i++) {
            result.clear();
            HDEBUGLOGD(eDebugResourceManager, "%d layer(%p) dump", i, display->mLayers[i]);
            display->mLayers[i]->printLayer();
            HDEBUGLOGD(eDebugResourceManager, "%s", result.c_str());
        }
    }

    if (!display->mUseDpu) {
        if ((ret = setClientTargetBufferToExynosCompositor(display)) != NO_ERROR) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: setClientTargetBufferToExynosCompositor) error (%d)",
                     __func__, ret);
            return ret;
        }
    }

    return NO_ERROR;
}

int ExynosResourceManager::setClientTargetBufferToExynosCompositor(ExynosDisplay *display) {
    int ret = NO_ERROR;

    if (display->mClientCompositionInfo.mHasCompositionLayer) {
        if (display->mExynosCompositionInfo.mM2mMPP == NULL) {
            HDEBUGLOGD(eDebugResourceManager, "mM2mMPP is NULL, set the ExynosMPP for blending");
            for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
                ExynosMPP *mpp = mM2mMPPs[i];

                if ((mpp->mMaxSrcLayerNum > 1) &&
                    (mpp->mPreAssignDisplayInfo & HWC_DISPLAY_VIRTUAL_BIT)) {
                    display->mExynosCompositionInfo.mM2mMPP = mpp;
                    break;
                }
            }
        }

        exynos_image src_img;
        exynos_image dst_img;
        display->setCompositionTargetExynosImage(COMPOSITION_CLIENT, &src_img, &dst_img);
        display->mClientCompositionInfo.setExynosImage(src_img, dst_img);
        display->mClientCompositionInfo.setExynosMidImage(dst_img);

        if ((ret = display->mExynosCompositionInfo.mM2mMPP->assignMPP(display->mDisplayInfo,
                                                                      &display->mClientCompositionInfo)) != NO_ERROR) {
            ALOGE("%s:: %s MPP assignMPP() error (%d)",
                  __func__, display->mExynosCompositionInfo.mM2mMPP->mName.c_str(), ret);
            return ret;
        }
        int prevHasCompositionLayer = display->mExynosCompositionInfo.mHasCompositionLayer;
        display->mExynosCompositionInfo.mHasCompositionLayer = true;
        // if prevHasCompositionLayer is false, setResourcePriority is not called
        if (prevHasCompositionLayer == false)
            setResourcePriority(display);
    }

    return ret;
}

int32_t ExynosResourceManager::setResourcePriority(ExynosDisplay *display) {
    int ret = NO_ERROR;
    int check_ret = NO_ERROR;
    ExynosMPP *m2mMPP = NULL;

    m2mMPP = display->mExynosCompositionInfo.mM2mMPP;

    if (m2mMPP == NULL || m2mMPP->mPhysicalType == MPP_MSC)
        return NO_ERROR;

    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];
        if ((layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) &&
            (layer->mM2mMPP != NULL) &&
            (layer->mM2mMPP->mPhysicalType == MPP_G2D) &&
            ((check_ret = layer->mM2mMPP->prioritize(2)) != NO_ERROR)) {
            if (check_ret < 0) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Fail to set exynoscomposition priority(%d)", ret);
            } else {
                m2mMPP = layer->mM2mMPP;
                layer->resetAssignedResource();
                layer->mOverlayInfo |= eResourcePendingWork;
                layer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
                ret = EXYNOS_ERROR_CHANGED;
                HDEBUGLOGD(eDebugResourceManager, "\t%s is disabled because of panding work",
                           m2mMPP->mName.c_str());
                m2mMPP->setDisableByUserScenario(DisableType::DISABLE_PRIORITY);
                layer->mCheckMPPFlag[m2mMPP->mLogicalType] = eMPPHWBusy;
            }
        }
    }

    m2mMPP = display->mExynosCompositionInfo.mM2mMPP;
    ExynosCompositionInfo &compositionInfo = display->mExynosCompositionInfo;
    if (compositionInfo.mHasCompositionLayer == true) {
        if ((m2mMPP == NULL) || (m2mMPP->mAcrylicHandle == NULL)) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "There is exynos composition layers but resource is null (%p)",
                     m2mMPP);
        } else if ((check_ret = m2mMPP->prioritize(2)) != NO_ERROR) {
            HDEBUGLOGD(eDebugResourceManager, "%s setting priority error(%d)", m2mMPP->mName.c_str(), check_ret);
            if (check_ret < 0) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Fail to set exynoscomposition priority(%d)", ret);
            } else {
                if (display->mExynosCompositionInfo.mFirstIndex >= 0) {
                    uint32_t firstIndex = (uint32_t)display->mExynosCompositionInfo.mFirstIndex;
                    uint32_t lastIndex = (uint32_t)display->mExynosCompositionInfo.mLastIndex;
                    for (uint32_t i = firstIndex; i <= lastIndex; i++) {
                        ExynosLayer *layer = display->mLayers[i];
                        layer->resetAssignedResource();
                        layer->mOverlayInfo |= eResourcePendingWork;
                        layer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
                        layer->mCheckMPPFlag[m2mMPP->mLogicalType] = eMPPHWBusy;
                    }
                }
                compositionInfo.initializeInfos();
                ret = EXYNOS_ERROR_CHANGED;
                m2mMPP->resetUsedCapacity();
                HDEBUGLOGD(eDebugResourceManager, "\t%s is disabled because of pending work",
                           m2mMPP->mName.c_str());
                m2mMPP->setDisableByUserScenario(DisableType::DISABLE_PRIORITY);
            }
        } else {
            HDEBUGLOGD(eDebugResourceManager, "%s setting priority is ok", m2mMPP->mName.c_str());
        }
    }

    return ret;
}

int32_t ExynosResourceManager::assignResourceInternal(ExynosDisplay *display) {
    int ret = NO_ERROR;
    int retry_count = 0;

    /*
     * First add layers that SF requested HWC2_COMPOSITION_CLIENT type
     * to client composition
     */
    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];
        if (layer->mCompositionType == HWC2_COMPOSITION_CLIENT) {
            layer->mOverlayInfo |= eSkipLayer;
            layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            if (((ret = display->addClientCompositionLayer(i)) != NO_ERROR) &&
                (ret != EXYNOS_ERROR_CHANGED)) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Handle HWC2_COMPOSITION_CLIENT type layers, but addClientCompositionLayer failed (%d)", ret);
                return ret;
            }
        }
    }

    do {
        HDEBUGLOGD(eDebugResourceAssigning, "%s:: retry_count(%d)", __func__, retry_count);
        if ((ret = resetAssignedResources(display)) != NO_ERROR)
            return ret;
        if ((ret = assignCompositionTarget(display, COMPOSITION_CLIENT)) != NO_ERROR) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: Fail to assign resource for compositionTarget",
                     __func__);
            return ret;
        }

        if ((ret = assignLayers(display, ePriorityMax)) != NO_ERROR) {
            if (ret == EXYNOS_ERROR_CHANGED) {
                retry_count++;
                continue;
            } else {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: Fail to assign resource for ePriorityMax layer",
                         __func__);
                return ret;
            }
        }

        if ((ret = assignLayers(display, ePriorityHigh)) != NO_ERROR) {
            if (ret == EXYNOS_ERROR_CHANGED) {
                retry_count++;
                continue;
            } else {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: Fail to assign resource for ePriorityHigh layer",
                         __func__);
                return ret;
            }
        }

        if ((ret = assignCompositionTarget(display, COMPOSITION_EXYNOS)) != NO_ERROR) {
            if (ret == eInsufficientMPP) {
                /*
                 * Change compositionTypes to HWC2_COMPOSITION_CLIENT
                 */
                uint32_t firstIndex = (uint32_t)display->mExynosCompositionInfo.mFirstIndex;
                uint32_t lastIndex = (uint32_t)display->mExynosCompositionInfo.mLastIndex;
                for (uint32_t i = firstIndex; i <= lastIndex; i++) {
                    ExynosLayer *layer = display->mLayers[i];
                    layer->resetAssignedResource();
                    layer->mOverlayInfo |= eInsufficientMPP;
                    layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
                    if (((ret = display->addClientCompositionLayer(i)) != NO_ERROR) &&
                        (ret != EXYNOS_ERROR_CHANGED)) {
                        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Change compositionTypes to HWC2_COMPOSITION_CLIENT, but addClientCompositionLayer failed (%d)", ret);
                        return ret;
                    }
                }
                display->mExynosCompositionInfo.initializeInfos();
                ret = EXYNOS_ERROR_CHANGED;
            } else {
                return ret;
            }
        }

        if (ret == NO_ERROR) {
            for (int32_t i = ePriorityHigh - 1; i > ePriorityNone; i--) {
                if ((ret = assignLayers(display, i)) == EXYNOS_ERROR_CHANGED)
                    break;
                if (ret != NO_ERROR)
                    return ret;
            }
        }

        /* Assignment is done */
        if (ret == NO_ERROR) {
            ret = setResourcePriority(display);
        }
        retry_count++;
    } while ((ret == EXYNOS_ERROR_CHANGED) && (retry_count < ASSIGN_RESOURCE_TRY_COUNT));

    if (retry_count == ASSIGN_RESOURCE_TRY_COUNT) {
        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: assign resources fail", __func__);
        ret = eUnknown;
        return ret;
    } else {
        if ((ret = updateExynosComposition(display)) != NO_ERROR)
            return ret;
        if ((ret = updateClientComposition(display)) != NO_ERROR)
            return ret;
    }

    if (hwcCheckDebugMessages(eDebugCapacity)) {
        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            if (mM2mMPPs[i]->mPhysicalType == MPP_G2D) {
                String8 dumpMPP;
                mM2mMPPs[i]->dump(dumpMPP);
                HDEBUGLOGD(eDebugCapacity, "%s", dumpMPP.c_str());
            }
        }
    }

    return ret;
}

int32_t ExynosResourceManager::updateExynosComposition(ExynosDisplay *display) {
    int ret = NO_ERROR;
    /* Use Exynos composition as many as possible */
    if ((display->mExynosCompositionInfo.mHasCompositionLayer == true) &&
        (display->mExynosCompositionInfo.mM2mMPP != NULL)) {
        if (display->mDisplayControl.useMaxG2DSrc == 1) {
            ExynosMPP *m2mMPP = display->mExynosCompositionInfo.mM2mMPP;
            uint32_t lastIndex = display->mExynosCompositionInfo.mLastIndex;
            uint32_t firstIndex = display->mExynosCompositionInfo.mFirstIndex;
            uint32_t remainNum = m2mMPP->mMaxSrcLayerNum - (lastIndex - firstIndex + 1);

            HDEBUGLOGD(eDebugResourceAssigning, "Update ExynosComposition firstIndex: %d, lastIndex: %d, remainNum: %d++++",
                       firstIndex, lastIndex, remainNum);

            ExynosLayer *layer = NULL;
            exynos_image src_img;
            exynos_image dst_img;
            if (remainNum > 0) {
                for (uint32_t i = (lastIndex + 1); i < display->mLayers.size(); i++) {
                    layer = display->mLayers[i];
                    layer->setSrcExynosImage(&src_img);
                    layer->setDstExynosImage(&dst_img);
                    layer->setExynosImage(src_img, dst_img);
                    bool isAssignableState = false;
                    if ((layer->mSupportedMPPFlag & m2mMPP->mLogicalType) != 0)
                        isAssignableState = isAssignable(m2mMPP, display, src_img, dst_img, layer);

                    bool canChange = (layer->mValidateCompositionType != HWC2_COMPOSITION_CLIENT) &&
                                     ((display->mDisplayControl.cursorSupport == false) ||
                                      (layer->mCompositionType != HWC2_COMPOSITION_CURSOR)) &&
                                     (layer->mSupportedMPPFlag & m2mMPP->mLogicalType) && isAssignableState;

                    HDEBUGLOGD(eDebugResourceAssigning, "\tlayer[%d] type: %d, 0x%8x, isAssignable: %d, canChange: %d, remainNum(%d)",
                               i, layer->mValidateCompositionType,
                               layer->mSupportedMPPFlag, isAssignableState, canChange, remainNum);
                    if (canChange) {
                        layer->resetAssignedResource();
                        layer->mOverlayInfo |= eUpdateExynosComposition;
                        if ((ret = m2mMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                            ALOGE("%s:: %s MPP assignMPP() error (%d)",
                                  __func__, m2mMPP->mName.c_str(), ret);
                            return ret;
                        }
                        layer->setExynosMidImage(dst_img);
                        float totalUsedCapacity = getResourceUsedCapa(*m2mMPP);
                        display->addExynosCompositionLayer(i, totalUsedCapacity);
                        layer->mValidateCompositionType = HWC2_COMPOSITION_EXYNOS;
                        remainNum--;
                    }
                    if ((canChange == false) || (remainNum == 0))
                        break;
                }
            }
            if (remainNum > 0) {
                for (int32_t i = (firstIndex - 1); i >= 0; i--) {
                    layer = display->mLayers[i];
                    layer->setSrcExynosImage(&src_img);
                    layer->setDstExynosImage(&dst_img);
                    layer->setExynosImage(src_img, dst_img);
                    bool isAssignableState = false;
                    if ((layer->mSupportedMPPFlag & m2mMPP->mLogicalType) != 0)
                        isAssignableState = isAssignable(m2mMPP, display, src_img, dst_img, layer);

                    bool canChange = (layer->mValidateCompositionType != HWC2_COMPOSITION_CLIENT) &&
                                     ((display->mDisplayControl.cursorSupport == false) ||
                                      (layer->mCompositionType != HWC2_COMPOSITION_CURSOR)) &&
                                     (layer->mSupportedMPPFlag & m2mMPP->mLogicalType) && isAssignableState;

                    HDEBUGLOGD(eDebugResourceAssigning, "\tlayer[%d] type: %d, 0x%8x, isAssignable: %d, canChange: %d, remainNum(%d)",
                               i, layer->mValidateCompositionType,
                               layer->mSupportedMPPFlag, isAssignableState, canChange, remainNum);
                    if (canChange) {
                        layer->resetAssignedResource();
                        layer->mOverlayInfo |= eUpdateExynosComposition;
                        if ((ret = m2mMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                            ALOGE("%s:: %s MPP assignMPP() error (%d)",
                                  __func__, m2mMPP->mName.c_str(), ret);
                            return ret;
                        }
                        layer->setExynosMidImage(dst_img);
                        float totalUsedCapacity = getResourceUsedCapa(*m2mMPP);
                        display->addExynosCompositionLayer(i, totalUsedCapacity);
                        layer->mValidateCompositionType = HWC2_COMPOSITION_EXYNOS;
                        remainNum--;
                    }
                    if ((canChange == false) || (remainNum == 0))
                        break;
                }
            }
            HDEBUGLOGD(eDebugResourceAssigning, "Update ExynosComposition firstIndex: %d, lastIndex: %d, remainNum: %d-----",
                       display->mExynosCompositionInfo.mFirstIndex, display->mExynosCompositionInfo.mLastIndex, remainNum);
        }

        /*
         * Check if there is only one exynos composition layer
         * Then it is not composition and m2mMPP is not required
         * if internalMPP can process the layer alone.
         */
        ExynosMPP *otfMPP = display->mExynosCompositionInfo.mOtfMPP;
        if ((display->mDisplayControl.enableExynosCompositionOptimization == true) &&
            (otfMPP != NULL) &&
            (display->mExynosCompositionInfo.mFirstIndex >= 0) &&
            (display->mExynosCompositionInfo.mFirstIndex == display->mExynosCompositionInfo.mLastIndex)) {
            ExynosLayer *layer = display->mLayers[display->mExynosCompositionInfo.mFirstIndex];
            if (layer->mSupportedMPPFlag & otfMPP->mLogicalType) {
                exynos_image src_img;
                layer->setSrcExynosImage(&src_img);
                /* mSupportedMPPFlag shows whether MPP HW can support the layer or not.
                   But we should consider additional AFBC condition affected by shared otfMPP. */
                if (otfMPP->isSupportedCompression(src_img)) {
                    layer->resetAssignedResource();
                    layer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
                    display->mExynosCompositionInfo.initializeInfos();
                    // reset otfMPP
                    if ((ret = otfMPP->resetAssignedState()) != NO_ERROR) {
                        ALOGE("%s:: %s MPP resetAssignedState() error (%d)",
                              __func__, otfMPP->mName.c_str(), ret);
                    }
                    // assign otfMPP again
                    if ((ret = otfMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                        ALOGE("%s:: %s MPP assignMPP() error (%d)",
                              __func__, otfMPP->mName.c_str(), ret);
                    }
                }
            }
        }
    }
    return ret;
}

int32_t ExynosResourceManager::changeLayerFromClientToDevice(ExynosDisplay *display, ExynosLayer *layer,
                                                             uint32_t layer_index, exynos_image &m2m_out_img, ExynosMPP *m2mMPP, ExynosMPP *otfMPP) {
    int ret = NO_ERROR;
    if ((ret = display->removeClientCompositionLayer(layer_index)) != NO_ERROR) {
        ALOGD("removeClientCompositionLayer return error(%d)", ret);
        return ret;
    }
    if (otfMPP != NULL) {
        if ((ret = otfMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
            ALOGE("%s:: %s MPP assignMPP() error (%d)",
                  __func__, otfMPP->mName.c_str(), ret);
            return ret;
        }
        HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: %s MPP is assigned",
                   layer_index, otfMPP->mName.c_str());
    }
    if (m2mMPP != NULL) {
        if ((ret = m2mMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
            ALOGE("%s:: %s MPP assignMPP() error (%d)",
                  __func__, m2mMPP->mName.c_str(), ret);
            return ret;
        }
        layer->setExynosMidImage(m2m_out_img);
        HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: %s MPP is assigned",
                   layer_index, m2mMPP->mName.c_str());
    }
    layer->mValidateCompositionType = HWC2_COMPOSITION_DEVICE;
    display->mWindowNumUsed++;
    HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: mWindowNumUsed(%d)",
               layer_index, display->mWindowNumUsed);

    return ret;
}
int32_t ExynosResourceManager::updateClientComposition(ExynosDisplay *display) {
    int ret = NO_ERROR;

    if (!display->mUseDpu)
        return ret;

    if ((exynosHWCControl.forceGpu == 1) ||
        (display->mClientCompositionInfo.mHasCompositionLayer == false))
        return ret;

    /* Check if there is layer that can be handled by overlay */
    int32_t firstIndex = display->mClientCompositionInfo.mFirstIndex;
    int32_t lastIndex = display->mClientCompositionInfo.mLastIndex;

#ifdef USE_DEDICATED_TOP_WINDOW
    /* Don't optimize If client composition is assigned with dedicated channel */
    if ((display->mClientCompositionInfo.mOtfMPP->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
        (display->mClientCompositionInfo.mOtfMPP->mPhysicalIndex == DEDICATED_CHANNEL_INDEX) &&
        (firstIndex == lastIndex))
        return ret;
#endif

    for (int32_t i = firstIndex; i <= lastIndex; i++) {
        ExynosMPP *m2mMPP = NULL;
        ExynosMPP *otfMPP = NULL;
        exynos_image m2m_out_img;
        uint32_t overlayInfo = 0;
        int32_t compositionType = 0;
        ExynosLayer *layer = display->mLayers[i];
        if ((layer->mOverlayPriority >= ePriorityHigh) &&
            (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)) {
            display->mClientCompositionInfo.mFirstIndex++;
            continue;
        }
        compositionType = assignLayer(display, layer, i, m2m_out_img, &m2mMPP, &otfMPP, overlayInfo);
        if (compositionType == HWC2_COMPOSITION_DEVICE) {
            /*
             * Don't allocate G2D
             * Execute can be fail because of other job
             * Prioritizing is required to allocate G2D
             */
            if ((m2mMPP != NULL) && (m2mMPP->mPhysicalType == MPP_G2D))
                break;

            if ((ret = changeLayerFromClientToDevice(display, layer, i, m2m_out_img, m2mMPP, otfMPP)) != NO_ERROR)
                return ret;
        } else {
            break;
        }
    }

#ifdef USE_DEDICATED_TOP_WINDOW
    /* Don't optimize If client composition is assigned with dedicated channel */
    if ((display->mClientCompositionInfo.mHasCompositionLayer) &&
        (display->mClientCompositionInfo.mOtfMPP->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
        (display->mClientCompositionInfo.mOtfMPP->mPhysicalIndex == DEDICATED_CHANNEL_INDEX))
        return ret;
#endif

    firstIndex = display->mClientCompositionInfo.mFirstIndex;
    lastIndex = display->mClientCompositionInfo.mLastIndex;
    for (int32_t i = lastIndex; i >= 0; i--) {
        ExynosMPP *m2mMPP = NULL;
        ExynosMPP *otfMPP = NULL;
        exynos_image m2m_out_img;
        uint32_t overlayInfo = 0;
        int32_t compositionType = 0;
        ExynosLayer *layer = display->mLayers[i];
        if ((layer->mOverlayPriority >= ePriorityHigh) &&
            (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE)) {
            display->mClientCompositionInfo.mLastIndex--;
            continue;
        }
        compositionType = assignLayer(display, layer, i, m2m_out_img, &m2mMPP, &otfMPP, overlayInfo);
        if (compositionType == HWC2_COMPOSITION_DEVICE) {
            /*
             * Don't allocate G2D
             * Execute can be fail because of other job
             * Prioritizing is required to allocate G2D
             */
            if ((m2mMPP != NULL) && (m2mMPP->mPhysicalType == MPP_G2D))
                break;
            if ((ret = changeLayerFromClientToDevice(display, layer, i, m2m_out_img, m2mMPP, otfMPP)) != NO_ERROR)
                return ret;
        } else {
            break;
        }
    }

    return ret;
}

int32_t ExynosResourceManager::resetAssignedResources(ExynosDisplay *display, bool forceReset) {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (display && (mOtfMPPs[i]->mAssignedDisplayInfo.displayIdentifier.id != display->mDisplayId))
            continue;

        mOtfMPPs[i]->resetAssignedState();
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (display && (mM2mMPPs[i]->mAssignedDisplayInfo.displayIdentifier.id != display->mDisplayId))
            continue;
        if ((forceReset == false) &&
            ((mM2mMPPs[i]->mMaxSrcLayerNum > 1))) {
            /*
             * Don't reset assigned state
             */
            continue;
        }
        mM2mMPPs[i]->resetAssignedState();
    }
    display->mWindowNumUsed = 0;

    return NO_ERROR;
}

int32_t ExynosResourceManager::assignCompositionTarget(ExynosDisplay *display, uint32_t targetType) {
    int32_t ret = NO_ERROR;
    ExynosCompositionInfo *compositionInfo;

    HDEBUGLOGD(eDebugResourceManager, "%s:: display(%d), targetType(%d) +++++",
               __func__, display->mType, targetType);

    if (targetType == COMPOSITION_CLIENT)
        compositionInfo = &(display->mClientCompositionInfo);
    else if (targetType == COMPOSITION_EXYNOS)
        compositionInfo = &(display->mExynosCompositionInfo);
    else
        return -EINVAL;

    if (compositionInfo->mHasCompositionLayer == false) {
        HDEBUGLOGD(eDebugResourceManager, "\tthere is no composition layers");
        return NO_ERROR;
    }

    exynos_image src_img;
    exynos_image dst_img;
    display->setCompositionTargetExynosImage(targetType, &src_img, &dst_img);

    if (targetType == COMPOSITION_EXYNOS) {
        if (compositionInfo->mM2mMPP == NULL) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: fail to assign M2mMPP (%d)", __func__, ret);
            return eInsufficientMPP;
        }
    }

    if ((compositionInfo->mFirstIndex < 0) ||
        (compositionInfo->mLastIndex < 0)) {
        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: layer index is not valid mFirstIndex(%d), mLastIndex(%d)",
                 __func__, compositionInfo->mFirstIndex, compositionInfo->mLastIndex);
        return -EINVAL;
    }

    if (display->mUseDpu == false) {
        return NO_ERROR;
    }

    int64_t isSupported = 0;
    bool isAssignableState = false;

    if ((display->mUseDpu) &&
        (display->mWindowNumUsed >= display->mMaxWindowNum))
        return eInsufficientMPP;

    otfMppReordering(display, mOtfMPPs, src_img, dst_img);

    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
#ifdef USE_DEDICATED_TOP_WINDOW
        if ((mOtfMPPs[i]->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
            (mOtfMPPs[i]->mPhysicalIndex == DEDICATED_CHANNEL_INDEX) &&
            (uint32_t)compositionInfo->mLastIndex != (display->mLayers.size() - 1))
            continue;
#endif

        compositionInfo->setExynosImage(src_img, dst_img);
        compositionInfo->setExynosMidImage(dst_img);
        HDEBUGLOGD(eDebugTDM, "%s M2M target calculation start", __func__);
        calculateHWResourceAmount(display, compositionInfo);

        isSupported = mOtfMPPs[i]->isSupported(display->mDisplayInfo, src_img, dst_img);
        if (isSupported == NO_ERROR)
            isAssignableState = isAssignable(mOtfMPPs[i], display, src_img, dst_img, compositionInfo);

        HDEBUGLOGD(eDebugResourceAssigning, "\t\t check %s: supportedBit(0x%" PRIx64 "), isAssignable(%d)",
                   mOtfMPPs[i]->mName.c_str(), -isSupported, isAssignableState);
        if ((isSupported == NO_ERROR) && (isAssignableState)) {
            if ((ret = mOtfMPPs[i]->assignMPP(display->mDisplayInfo, compositionInfo)) != NO_ERROR) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: %s MPP assignMPP() error (%d)",
                         __func__, mOtfMPPs[i]->mName.c_str(), ret);
                return ret;
            }
            compositionInfo->mOtfMPP = mOtfMPPs[i];
            display->mWindowNumUsed++;

            HDEBUGLOGD(eDebugResourceManager, "%s:: %s is assigned", __func__, mOtfMPPs[i]->mName.c_str());
            return NO_ERROR;
        }
    }

    HDEBUGLOGD(eDebugResourceManager, "%s:: insufficient MPP", __func__);
    return eInsufficientMPP;
}

int32_t ExynosResourceManager::validateLayer(uint32_t index, ExynosDisplay *display, ExynosLayer *layer) {
    if ((layer == NULL) || (display == NULL))
        return eUnknown;

#ifdef ENABLE_FORCE_GPU_FOR_BOOTANIM
    char value[256];
    int bootanim_exit;
    if (property_get("service.bootanim.progress", value, "")) {
        property_get("service.bootanim.exit", value, "0");
        bootanim_exit = atoi(value);

        if (!bootanim_exit) {
            return eForceFbEnabled;
        }
    }
#endif

    if (exynosHWCControl.forceGpu == 1) {
        return eForceFbEnabled;
    }

    if (layer->mLayerFlag & EXYNOS_HWC_FORCE_CLIENT_WFD)
        return eFroceClientLayer;
    if (layer->mLayerFlag & EXYNOS_HWC_FORCE_CLIENT_HDR_META_ERROR)
        return eInvalidVideoMetaData;

    if ((display->mLayers.size() >= MAX_OVERLAY_LAYER_NUM) &&
        (layer->mOverlayPriority < ePriorityHigh))
        return eExceedMaxLayerNum;

    if (display->mUseDynamicRecomp &&
        (display->mDynamicRecompMode == DEVICE_TO_CLIENT))
        return eDynamicRecomposition;

    if (layer->mCompositionType == HWC2_COMPOSITION_CLIENT)
        return eSkipLayer;

#ifndef HWC_SUPPORT_COLOR_TRANSFORM
    if (display->mColorTransformHint != HAL_COLOR_TRANSFORM_IDENTITY)
        return eUnSupportedColorTransform;
#else
    if ((display->mColorTransformHint < 0) &&
        (layer->mOverlayPriority < ePriorityHigh))
        return eUnSupportedColorTransform;
#endif

    if (layer->isDimLayer()) {
        return eDimLayer;
    }

    if (layer->mLayerBuffer == NULL)
        return eInvalidHandle;
    if (isSrcCropFloat(layer->mPreprocessedInfo.sourceCrop))
        return eHasFloatSrcCrop;

    if ((layer->mPreprocessedInfo.displayFrame.left < 0) ||
        (layer->mPreprocessedInfo.displayFrame.top < 0) ||
        (layer->mPreprocessedInfo.displayFrame.right > (int32_t)display->mXres) ||
        (layer->mPreprocessedInfo.displayFrame.bottom > (int32_t)display->mYres))
        return eInvalidDispFrame;

    return NO_ERROR;
}

int32_t ExynosResourceManager::getCandidateM2mMPPOutImages(ExynosDisplay *display,
                                                           ExynosLayer *layer, std::vector<exynos_image> &image_lists) {
    exynos_image src_img;
    exynos_image dst_img;
    layer->setSrcExynosImage(&src_img);
    layer->setDstExynosImage(&dst_img);
    dst_img.transform = 0;
    /* Position is (0, 0) */
    dst_img.x = 0;
    dst_img.y = 0;

    /* Check original source format first */
    dst_img.exynosFormat = src_img.exynosFormat;
    dst_img.dataSpace = src_img.dataSpace;

    /* Copy origin source HDR metadata */
    dst_img.metaParcel = src_img.metaParcel;

    uint32_t dstW = dst_img.w;
    uint32_t dstH = dst_img.h;
    bool isPerpendicular = !!(src_img.transform & HAL_TRANSFORM_ROT_90);
    if (isPerpendicular) {
        dstW = dst_img.h;
        dstH = dst_img.w;
    }

    /* Scale up case */
    if ((dstW > src_img.w) && (dstH > src_img.h)) {
        /* otfMPP doesn't rotate image, m2mMPP rotates image */
        src_img.transform = 0;
        exynos_image dst_scale_img = dst_img;

        ExynosMPP *otfMppForScale = nullptr;
        auto mpp_it = std::find_if(mOtfMPPs.begin(), mOtfMPPs.end(),
                                   [&src_img, &dst_scale_img](auto m) {
                                       return (m->getMaxUpscale(src_img, dst_scale_img) > 1);
                                   });
        otfMppForScale = mpp_it == mOtfMPPs.end() ? nullptr : *mpp_it;

        if (hasHdrInfo(src_img)) {
            if (src_img.exynosFormat.isYUV())
                dst_scale_img.exynosFormat = PredefinedFormat::exynosFormatP010;
            else
                dst_scale_img.exynosFormat = PredefinedFormat::exynosFormatRgba10;
        } else {
            if (src_img.exynosFormat.isYUV())
                dst_scale_img.exynosFormat = ExynosMPP::defaultMppDstYuvFormat;
            else
                dst_scale_img.exynosFormat = ExynosMPP::defaultMppDstFormat;
        }

        if (otfMppForScale) {
            uint32_t upScaleRatio = otfMppForScale->getMaxUpscale(src_img, dst_scale_img);
            uint32_t downScaleRatio = otfMppForScale->getMaxDownscale(display->mDisplayInfo, src_img, dst_scale_img);
            uint32_t srcCropWidthAlign = otfMppForScale->getSrcCropWidthAlign(src_img);
            uint32_t srcCropHeightAlign = otfMppForScale->getSrcCropHeightAlign(src_img);

            dst_scale_img.x = 0;
            dst_scale_img.y = 0;
            if (isPerpendicular) {
                dst_scale_img.w = pixel_align(src_img.h, srcCropWidthAlign);
                dst_scale_img.h = pixel_align(src_img.w, srcCropHeightAlign);
            } else {
                dst_scale_img.w = pixel_align(src_img.w, srcCropWidthAlign);
                dst_scale_img.h = pixel_align(src_img.h, srcCropHeightAlign);
            }

            HDEBUGLOGD(eDebugResourceAssigning, "scale up w: %d, h: %d, ratio(type: %d, %d, %d)",
                       dst_scale_img.w, dst_scale_img.h,
                       otfMppForScale->mLogicalType, upScaleRatio, downScaleRatio);
            if (dst_scale_img.w * upScaleRatio < dst_img.w) {
                dst_scale_img.w = pixel_align((uint32_t)ceilf((float)dst_img.w / (float)upScaleRatio), srcCropWidthAlign);
            }
            if (dst_scale_img.h * upScaleRatio < dst_img.h) {
                dst_scale_img.h = pixel_align((uint32_t)ceilf((float)dst_img.h / (float)upScaleRatio), srcCropHeightAlign);
            }
            HDEBUGLOGD(eDebugResourceAssigning, "\tsrc[%d, %d, %d,%d], dst[%d, %d, %d,%d], mid[%d, %d, %d, %d]",
                       src_img.x, src_img.y, src_img.w, src_img.h,
                       dst_img.x, dst_img.y, dst_img.w, dst_img.h,
                       dst_scale_img.x, dst_scale_img.y, dst_scale_img.w, dst_scale_img.h);
            image_lists.push_back(dst_scale_img);

            if (dst_scale_img.exynosFormat.isSBWC()) {
                /*
                 * SBWC format could not be supported in specific dst size
                 * Add uncompressed YUV format to cover this size
                 */
                dst_scale_img.exynosFormat = ExynosMPP::defaultMppDstUncompYuvFormat;
                image_lists.push_back(dst_scale_img);
            }
        }
    }

    if (src_img.exynosFormat.isYUV() && !hasHdrInfo(src_img)) {
        dst_img.exynosFormat = ExynosMPP::defaultMppDstYuvFormat;
    }

    /* For HDR through MSC or G2D case but dataspace is not changed */
    if (hasHdrInfo(src_img)) {
        if (src_img.exynosFormat.isYUV())
            dst_img.exynosFormat = PredefinedFormat::exynosFormatP010;
        else
            dst_img.exynosFormat = PredefinedFormat::exynosFormatRgba10;
        dst_img.dataSpace = src_img.dataSpace;

        ExynosMPP *otfMPP = getHDR10OtfMPP();
        uint32_t origin_dst_w = dst_img.w;
        uint32_t origin_dst_h = dst_img.h;
        uint32_t srcCropWidthAlign = 1;
        uint32_t srcCropHeightAlign = 1;
        if (otfMPP != NULL) {
            srcCropWidthAlign = otfMPP->getSrcCropWidthAlign(dst_img);
            srcCropHeightAlign = otfMPP->getSrcCropHeightAlign(dst_img);
        }
        dst_img.w = pixel_align(dst_img.w, srcCropWidthAlign);
        dst_img.h = pixel_align(dst_img.h, srcCropHeightAlign);

        if ((src_img.exynosFormat.isYUV()) && (src_img.exynosFormat.is10Bit())) {
            image_lists.push_back(dst_img);
            dst_img.exynosFormat = PredefinedFormat::exynosFormatRgba10;
            dst_img.w = origin_dst_w;
            dst_img.h = origin_dst_h;
        }
    }
    /*for 2K video, when format is RGB565, scale is needed, we use RGBA8888 output format to get better quality.
        This can avoid the striped artifact for gradually changing color region*/
    if(src_img.exynosFormat == HAL_PIXEL_FORMAT_RGB_565 && dst_img.exynosFormat == HAL_PIXEL_FORMAT_RGB_565){
        /* Scale amount */
        int srcW = src_img.w;
        int srcH = src_img.h;
        int dstW = dst_img.w;
        int dstH = dst_img.h;
        if (!!(src_img.transform & HAL_TRANSFORM_ROT_90)) {
            int tmp = dstW;
            dstW = dstH;
            dstH = tmp;
        }
        bool isScaled = ((srcW != dstW) || (srcH != dstH));
        if(isScaled)
            dst_img.exynosFormat = ExynosMPP::defaultMppDstFormat;
    }

    image_lists.push_back(dst_img);
    if (dst_img.exynosFormat.isSBWC()) {
        /*
         * SBWC format could not be supported in specific dst size
         * Add uncompressed YUV format to cover this size
         */
        dst_img.exynosFormat = ExynosMPP::defaultMppDstUncompYuvFormat;
        image_lists.push_back(dst_img);
    }

    /* For G2D HDR case */
    if (hasHdrInfo(src_img)) {
        if ((display->mType == HWC_DISPLAY_EXTERNAL) &&
            (display->mPlugState) && (((ExynosExternalDisplay *)display)->mSinkHdrSupported)) {
            dst_img.exynosFormat = PredefinedFormat::exynosFormatRgba10;
            dst_img.dataSpace = src_img.dataSpace;
        } else {
            uint32_t dataspace = HAL_DATASPACE_UNKNOWN;
            if (display->mColorMode == HAL_COLOR_MODE_NATIVE) {
                dataspace = HAL_DATASPACE_DCI_P3;
                dataspace &= ~HAL_DATASPACE_TRANSFER_MASK;
                dataspace |= HAL_DATASPACE_TRANSFER_GAMMA2_2;
                dataspace &= ~HAL_DATASPACE_RANGE_MASK;
                dataspace |= HAL_DATASPACE_RANGE_LIMITED;
            } else {
                dataspace = colorModeToDataspace(display->mColorMode);
            }
            dst_img.exynosFormat = PredefinedFormat::exynosFormatRgba10;
            dst_img.dataSpace = (android_dataspace)dataspace;
        }

        image_lists.push_back(dst_img);
    }

    if (src_img.exynosFormat.isYUV() && !hasHdrInfo(src_img)) {
        /* Check RGB format */
        dst_img.exynosFormat = ExynosMPP::defaultMppDstFormat;
        if (display->mColorMode == HAL_COLOR_MODE_NATIVE) {
            /* Bypass dataSpace */
            dst_img.dataSpace = src_img.dataSpace;
        } else {
            /* Covert data space */
            dst_img.dataSpace = colorModeToDataspace(display->mColorMode);
        }
        image_lists.push_back(dst_img);
    }

    /*
     * image_lists[] would be src of otfMPP.
     * Layer color transform should be addressed
     * with dataspace conversion.
     * It should be addressed by m2mMPP if m2mMPP converts dataspace.
     * In other cases, m2mMPP ignores color transform setting and
     * otfMPP addresses layer color transform if it is necessary.
     */
    for (auto &image : image_lists) {
        if (image.dataSpace == src_img.dataSpace)
            image.needColorTransform = src_img.needColorTransform;
        else
            image.needColorTransform = false;
    }

    return static_cast<int32_t>(image_lists.size());
}

int32_t ExynosResourceManager::assignLayer(ExynosDisplay *display, ExynosLayer *layer, uint32_t layer_index,
                                           exynos_image &m2m_out_img, ExynosMPP **m2mMPP, ExynosMPP **otfMPP, uint32_t &overlayInfo) {
    int32_t ret = NO_ERROR;
    uint32_t validateFlag = 0;

    exynos_image src_img;
    exynos_image dst_img;
    layer->setSrcExynosImage(&src_img);
    layer->setDstExynosImage(&dst_img);
    layer->setExynosImage(src_img, dst_img);
    layer->setExynosMidImage(dst_img);

    validateFlag = validateLayer(layer_index, display, layer);
    if ((display->mUseDpu) &&
        (display->mWindowNumUsed >= display->mMaxWindowNum))
        validateFlag |= eInsufficientWindow;

    HDEBUGLOGD(eDebugResourceManager, "\t[%d] layer: validateFlag(0x%8x), supportedMPPFlag(0x%8x), mLayerFlag(0x%8x)",
               layer_index, validateFlag, layer->mSupportedMPPFlag, layer->mLayerFlag);

    if (hwcCheckDebugMessages(eDebugResourceAssigning)) {
        layer->printLayer();
    }

    if ((validateFlag == NO_ERROR) || (validateFlag & eInsufficientWindow) ||
        (validateFlag & eDimLayer)) {
        bool isAssignableFlag = false;
        uint64_t isSupported = 0;
        /* 1. Find available otfMPP */
        if ((display->mUseDpu) &&
            (!(validateFlag & eInsufficientWindow))) {
            otfMppReordering(display, mOtfMPPs, src_img, dst_img);

            for (uint32_t j = 0; j < mOtfMPPs.size(); j++) {
#ifdef USE_DEDICATED_TOP_WINDOW
                if ((mOtfMPPs[j]->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
                    (mOtfMPPs[j]->mPhysicalIndex == DEDICATED_CHANNEL_INDEX) &&
                    (uint32_t)layer_index != (display->mLayers.size() - 1))
                    continue;
#endif
                if ((layer->mSupportedMPPFlag & mOtfMPPs[j]->mLogicalType) != 0)
                    isAssignableFlag = isAssignable(mOtfMPPs[j], display, src_img, dst_img, layer);

                HDEBUGLOGD(eDebugResourceAssigning, "\t\t check %s: flag (%d) supportedBit(%d), isAssignable(%d)",
                           mOtfMPPs[j]->mName.c_str(), layer->mSupportedMPPFlag,
                           (layer->mSupportedMPPFlag & mOtfMPPs[j]->mLogicalType), isAssignableFlag);

                if ((layer->mSupportedMPPFlag & mOtfMPPs[j]->mLogicalType) && (isAssignableFlag)) {
                    isSupported = mOtfMPPs[j]->isSupported(display->mDisplayInfo, src_img, dst_img);
                    HDEBUGLOGD(eDebugResourceAssigning, "\t\t\t isSuported(%" PRIx64 ")", -isSupported);
                    if (isSupported == NO_ERROR) {
                        *otfMPP = mOtfMPPs[j];
                        return HWC2_COMPOSITION_DEVICE;
                    }
                }
            }
        }

        /* 2. Find available m2mMPP */
        for (uint32_t j = 0; j < mM2mMPPs.size(); j++) {
            if ((display->mUseDpu == true) &&
                ((mM2mMPPs[j]->mLogicalType == MPP_LOGICAL_G2D_COMBO) ||
                 (mM2mMPPs[j]->mLogicalType == MPP_LOGICAL_MSC_COMBO)))
                continue;
            if ((display->mUseDpu == false) &&
                (mM2mMPPs[j]->mLogicalType == MPP_LOGICAL_G2D_RGB))
                continue;

            /* Only G2D can be assigned if layer is supported by G2D
             * when window is not sufficient
             */
            if ((validateFlag & eInsufficientWindow) &&
                (!(mM2mMPPs[j]->mMaxSrcLayerNum > 1))) {
                HDEBUGLOGD(eDebugResourceAssigning, "\t\tInsufficient window but exynosComposition is not assigned");
                continue;
            }

            bool isAssignableState = mM2mMPPs[j]->isAssignableState(display->mDisplayInfo, src_img, dst_img);

            HDEBUGLOGD(eDebugResourceAssigning, "\t\t check %s: supportedBit(%d), isAssignableState(%d)",
                       mM2mMPPs[j]->mName.c_str(),
                       (layer->mSupportedMPPFlag & mM2mMPPs[j]->mLogicalType), isAssignableState);

            float totalUsedCapa = ExynosResourceManager::getResourceUsedCapa(*mM2mMPPs[j]);
            if (isAssignableState) {
                if (!(mM2mMPPs[j]->mMaxSrcLayerNum > 1)) {
                    exynos_image otf_dst_img = dst_img;

                    otf_dst_img.exynosFormat = ExynosMPP::defaultMppDstFormat;

                    std::vector<exynos_image> image_lists;
                    if ((ret = getCandidateM2mMPPOutImages(display, layer, image_lists)) < 0) {
                        HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Fail getCandidateM2mMPPOutImages (%d)", ret);
                        return ret;
                    }
                    HDEBUGLOGD(eDebugResourceAssigning, "candidate M2mMPPOutImage num: %zu", image_lists.size());
                    for (auto &otf_src_img : image_lists) {
                        dumpExynosImage(eDebugResourceAssigning, otf_src_img);
                        exynos_image m2m_src_img = src_img;
                        /* transform is already handled by m2mMPP */
                        otf_src_img.transform = 0;
                        otf_dst_img.transform = 0;

                        /*
                         * This is the case that layer color transform should be
                         * addressed by otfMPP not m2mMPP
                         */
                        if (otf_src_img.needColorTransform)
                            m2m_src_img.needColorTransform = false;

                        if (((isSupported = mM2mMPPs[j]->isSupported(display->mDisplayInfo, m2m_src_img, otf_src_img)) != NO_ERROR) ||
                            ((isAssignableFlag = mM2mMPPs[j]->hasEnoughCapa(display->mDisplayInfo, m2m_src_img, otf_src_img, totalUsedCapa)) == false)) {
                            HDEBUGLOGD(eDebugResourceAssigning, "\t\t\t check %s: supportedBit(0x%" PRIx64 "), hasEnoughCapa(%d)",
                                       mM2mMPPs[j]->mName.c_str(), -isSupported, isAssignableFlag);
                            continue;
                        }

                        otfMppReordering(display, mOtfMPPs, otf_src_img, otf_dst_img);

                        /* 3. Find available OtfMPP for output of m2mMPP */
                        for (uint32_t k = 0; k < mOtfMPPs.size(); k++) {
#ifdef USE_DEDICATED_TOP_WINDOW
                            if ((mOtfMPPs[k]->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
                                (mOtfMPPs[k]->mPhysicalIndex == DEDICATED_CHANNEL_INDEX) &&
                                (uint32_t)layer_index != (display->mLayers.size() - 1))
                                continue;
#endif

                            isSupported = mOtfMPPs[k]->isSupported(display->mDisplayInfo, otf_src_img, otf_dst_img);
                            isAssignableFlag = false;
                            if (isSupported == NO_ERROR) {
                                /* to prevent HW resource execeeded */
                                ExynosCompositionInfo dpuSrcInfo;
                                dpuSrcInfo.mSrcImg = otf_src_img;
                                dpuSrcInfo.mDstImg = otf_dst_img;
                                HDEBUGLOGD(eDebugTDM, "%s Composition target calculation start (candidates)", __func__);
                                calculateHWResourceAmount(display, &dpuSrcInfo);

                                isAssignableFlag = isAssignable(mOtfMPPs[k], display, otf_src_img, otf_dst_img, &dpuSrcInfo);
                            }

                            HDEBUGLOGD(eDebugResourceAssigning, "\t\t\t check %s: supportedBit(0x%" PRIx64 "), isAssignable(%d)",
                                       mOtfMPPs[k]->mName.c_str(), -isSupported, isAssignableFlag);
                            if ((isSupported == NO_ERROR) && isAssignableFlag) {
                                *m2mMPP = mM2mMPPs[j];
                                *otfMPP = mOtfMPPs[k];
                                m2m_out_img = otf_src_img;
                                return HWC2_COMPOSITION_DEVICE;
                            }
                        }
                    }
                } else {
                    if ((layer->mSupportedMPPFlag & mM2mMPPs[j]->mLogicalType) &&
                        ((isAssignableFlag = mM2mMPPs[j]->hasEnoughCapa(display->mDisplayInfo, src_img, dst_img, totalUsedCapa) == true))) {
                        *m2mMPP = mM2mMPPs[j];
                        return HWC2_COMPOSITION_EXYNOS;
                    } else {
                        HDEBUGLOGD(eDebugResourceManager, "\t\t\t check %s: layer's mSupportedMPPFlag(0x%8x), hasEnoughCapa(%d)",
                                   mM2mMPPs[j]->mName.c_str(), layer->mSupportedMPPFlag, isAssignableFlag);
                    }
                }
            }
        }
    }
    /* Fail to assign resource */
    if (validateFlag != NO_ERROR)
        overlayInfo = validateFlag;
    else
        overlayInfo = eMPPUnsupported;

    /* If HDR10plus is failed to assign resource, change to HDR10 and reassign resource */
    if (hasHdr10Plus(src_img)) {
        /* If layer has to be processed by GPU, it doesn't need to regard as HDR10 */
        if ((layer->mOverlayInfo & eSkipLayer) || (layer->clearHdrDynamicType() != NO_ERROR))
            return HWC2_COMPOSITION_CLIENT;

        overlayInfo = eRemoveDynamicMetadata;
        HDEBUGLOGD(eDebugResourceManager,
                   "%s::[%d] layer's hdr info is changed, so need to reassign resource", __func__, layer_index);
    }

    return HWC2_COMPOSITION_CLIENT;
}

int32_t ExynosResourceManager::assignLayers(ExynosDisplay *display, uint32_t priority) {
    HDEBUGLOGD(eDebugResourceAssigning, "%s:: display(%d), priority(%d) +++++",
               __func__, display->mType, priority);

    int32_t ret = NO_ERROR;
    bool needReAssign = false;
    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];
        ExynosMPP *m2mMPP = NULL;
        ExynosMPP *otfMPP = NULL;
        exynos_image m2m_out_img;
        uint32_t validateFlag = 0;
        int32_t compositionType = 0;

        if ((layer->mValidateCompositionType == HWC2_COMPOSITION_CLIENT) ||
            (layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS))
            continue;
        if (layer->mOverlayPriority != priority)
            continue;

        exynos_image src_img;
        exynos_image dst_img;
        layer->setSrcExynosImage(&src_img);
        layer->setDstExynosImage(&dst_img);
        layer->setExynosImage(src_img, dst_img);
        layer->setExynosMidImage(dst_img);

        compositionType = assignLayer(display, layer, i, m2m_out_img, &m2mMPP, &otfMPP, validateFlag);
        if (compositionType == HWC2_COMPOSITION_DEVICE) {
            if (otfMPP != NULL) {
                if ((ret = otfMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                    ALOGE("%s:: %s MPP assignMPP() error (%d)",
                          __func__, otfMPP->mName.c_str(), ret);
                    return ret;
                }
                HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: %s MPP is assigned",
                           i, otfMPP->mName.c_str());
            }
            if (m2mMPP != NULL) {
                if ((ret = m2mMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                    ALOGE("%s:: %s MPP assignMPP() error (%d)",
                          __func__, m2mMPP->mName.c_str(), ret);
                    return ret;
                }
                layer->setExynosMidImage(m2m_out_img);
                HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: %s MPP is assigned",
                           i, m2mMPP->mName.c_str());
            }

            layer->mValidateCompositionType = compositionType;
            display->mWindowNumUsed++;
            HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: mWindowNumUsed(%d)",
                       i, display->mWindowNumUsed);
        } else if (compositionType == HWC2_COMPOSITION_EXYNOS) {
            float totalUsedCapacity = 0;
            if (m2mMPP != NULL) {
                if ((ret = m2mMPP->assignMPP(display->mDisplayInfo, layer)) != NO_ERROR) {
                    ALOGE("%s:: %s MPP assignMPP() error (%d)",
                          __func__, m2mMPP->mName.c_str(), ret);
                    return ret;
                }
                totalUsedCapacity = getResourceUsedCapa(*m2mMPP);
                HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: %s MPP is assigned",
                           i, m2mMPP->mName.c_str());
            }
            layer->mValidateCompositionType = compositionType;

            HDEBUGLOGD(eDebugResourceAssigning, "\t\t[%d] layer: exynosComposition", i);
            /* G2D composition */
            if (((ret = display->addExynosCompositionLayer(i, totalUsedCapacity)) == EXYNOS_ERROR_CHANGED) ||
                (ret < 0))
                return ret;
        } else {
            /* Layer's hdr info is changed, reassign resources */
            if (validateFlag & eRemoveDynamicMetadata) {
                needReAssign = true;
                break;
            }

            /* Fail to assign resource, set HWC2_COMPOSITION_CLIENT */
            if (validateFlag != NO_ERROR)
                layer->mOverlayInfo |= validateFlag;
            else
                layer->mOverlayInfo |= eMPPUnsupported;

            layer->mValidateCompositionType = HWC2_COMPOSITION_CLIENT;
            if (((ret = display->addClientCompositionLayer(i)) == EXYNOS_ERROR_CHANGED) ||
                (ret < 0))
                return ret;
        }
    }
    if (needReAssign) {
        if ((display->mClientCompositionInfo.mHasCompositionLayer) &&
            (display->mClientCompositionInfo.mOtfMPP != NULL))
            display->mClientCompositionInfo.mOtfMPP->resetAssignedState();

        if (display->mExynosCompositionInfo.mHasCompositionLayer) {
            if (display->mExynosCompositionInfo.mOtfMPP != NULL)
                display->mExynosCompositionInfo.mOtfMPP->resetAssignedState();
            if (display->mExynosCompositionInfo.mM2mMPP != NULL)
                display->mExynosCompositionInfo.mM2mMPP->resetAssignedState();
        }

        display->initializeValidateInfos();
        return EXYNOS_ERROR_CHANGED;
    }
    return ret;
}

int32_t ExynosResourceManager::assignWindow(ExynosDisplay *display) {
    HDEBUGLOGD(eDebugResourceManager, "%s +++++", __func__);
    int ret = NO_ERROR;
    uint32_t windowIndex = 0;

    if (!display->mUseDpu)
        return ret;

    windowIndex = display->mBaseWindowIndex;
    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];
        HDEBUGLOGD(eDebugResourceAssigning, "\t[%d] layer type: %d", i, layer->mValidateCompositionType);

        if (layer->mValidateCompositionType == HWC2_COMPOSITION_DEVICE) {
#ifdef USE_DEDICATED_TOP_WINDOW
            if ((layer->mOtfMPP) &&
                (layer->mOtfMPP->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
                (layer->mOtfMPP->mPhysicalIndex == DEDICATED_CHANNEL_INDEX)) {
                layer->mWindowIndex = MAX_DECON_WIN - 1;
                HDEBUGLOGD(eDebugResourceManager, "\t\t[%d] layer windowIndex: %d", i, MAX_DECON_WIN - 1);
                continue;
            }
#endif
            layer->mWindowIndex = windowIndex;
            HDEBUGLOGD(eDebugResourceManager, "\t\t[%d] layer windowIndex: %d", i, windowIndex);
        } else if ((layer->mValidateCompositionType == HWC2_COMPOSITION_CLIENT) ||
                   (layer->mValidateCompositionType == HWC2_COMPOSITION_EXYNOS)) {
            ExynosCompositionInfo *compositionInfo;
            if (layer->mValidateCompositionType == HWC2_COMPOSITION_CLIENT)
                compositionInfo = &display->mClientCompositionInfo;
            else
                compositionInfo = &display->mExynosCompositionInfo;

            if ((compositionInfo->mHasCompositionLayer == false) ||
                (compositionInfo->mFirstIndex < 0) ||
                (compositionInfo->mLastIndex < 0)) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: Invalid %s CompositionInfo mHasCompositionLayer(%d), "
                                                                  "mFirstIndex(%d), mLastIndex(%d) ",
                         __func__, compositionInfo->getTypeStr().c_str(),
                         compositionInfo->mHasCompositionLayer,
                         compositionInfo->mFirstIndex,
                         compositionInfo->mLastIndex);
                continue;
            }
            if (i != (uint32_t)compositionInfo->mLastIndex)
                continue;
#ifdef USE_DEDICATED_TOP_WINDOW
            if ((compositionInfo->mOtfMPP) &&
                (compositionInfo->mOtfMPP->mPhysicalType == DEDICATED_CHANNEL_TYPE) &&
                (compositionInfo->mOtfMPP->mPhysicalIndex == DEDICATED_CHANNEL_INDEX)) {
                compositionInfo->mWindowIndex = MAX_DECON_WIN - 1;
                HDEBUGLOGD(eDebugResourceManager, "\t\t[%d] %s Composition windowIndex: %d",
                           i, compositionInfo->getTypeStr().c_str(), MAX_DECON_WIN - 1);
                continue;
            }
#endif
            compositionInfo->mWindowIndex = windowIndex;
            HDEBUGLOGD(eDebugResourceManager, "\t\t[%d] %s Composition windowIndex: %d",
                       i, compositionInfo->getTypeStr().c_str(), windowIndex);
        } else {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: Invalid layer compositionType layer(%d), compositionType(%d)",
                     __func__, i, layer->mValidateCompositionType);
            continue;
        }
        windowIndex++;
    }
    HDEBUGLOGD(eDebugResourceManager, "%s ------", __func__);
    return ret;
}

/**
 * @param * display
 * @return int
 */
int32_t ExynosResourceManager::updateSupportedMPPFlag(ExynosDisplay *display) {
    int64_t ret = 0;
    HDEBUGLOGD(eDebugResourceAssigning, "%s++++++++++", __func__);
    for (uint32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];
        HDEBUGLOGD(eDebugResourceAssigning, "[%d] layer ", i);

        if (layer->mGeometryChanged == 0)
            continue;

        exynos_image src_img;
        exynos_image dst_img;
        exynos_image dst_img_yuv;
        layer->setSrcExynosImage(&src_img);
        layer->setDstExynosImage(&dst_img);
        layer->setDstExynosImage(&dst_img_yuv);
        dst_img.exynosFormat = ExynosMPP::defaultMppDstFormat;
        dst_img_yuv.exynosFormat = ExynosMPP::defaultMppDstYuvFormat;
        HDEBUGLOGD(eDebugResourceAssigning, "\tsrc_img");
        dumpExynosImage(eDebugResourceAssigning, src_img);
        HDEBUGLOGD(eDebugResourceAssigning, "\tdst_img");
        dumpExynosImage(eDebugResourceAssigning, dst_img);

        /* Initialize flags */
        layer->mSupportedMPPFlag = 0;
        layer->mCheckMPPFlag.clear();

        /* Check OtfMPPs */
        for (uint32_t j = 0; j < mOtfMPPs.size(); j++) {
            if ((ret = mOtfMPPs[j]->isSupported(display->mDisplayInfo, src_img, dst_img)) == NO_ERROR) {
                layer->mSupportedMPPFlag |= mOtfMPPs[j]->mLogicalType;
                HDEBUGLOGD(eDebugResourceAssigning, "\t%s: supported", mOtfMPPs[j]->mName.c_str());
            } else {
                if (((-ret) == eMPPUnsupportedFormat) &&
                    ((ret = mOtfMPPs[j]->isSupported(display->mDisplayInfo, src_img, dst_img_yuv)) == NO_ERROR)) {
                    layer->mSupportedMPPFlag |= mOtfMPPs[j]->mLogicalType;
                    HDEBUGLOGD(eDebugResourceAssigning, "\t%s: supported with yuv dst", mOtfMPPs[j]->mName.c_str());
                }
            }
            if (ret < 0) {
                HDEBUGLOGD(eDebugResourceAssigning, "\t%s: unsupported flag(0x%" PRIx64 ")", mOtfMPPs[j]->mName.c_str(), -ret);
                uint64_t checkFlag = 0x0;
                if (layer->mCheckMPPFlag.find(mOtfMPPs[j]->mLogicalType) !=
                    layer->mCheckMPPFlag.end()) {
                    checkFlag = layer->mCheckMPPFlag.at(mOtfMPPs[j]->mLogicalType);
                }
                checkFlag |= (-ret);
                layer->mCheckMPPFlag[mOtfMPPs[j]->mLogicalType] = checkFlag;
            }
        }

        /* Check M2mMPPs */
        for (uint32_t j = 0; j < mM2mMPPs.size(); j++) {
            if ((ret = mM2mMPPs[j]->isSupported(display->mDisplayInfo, src_img, dst_img)) == NO_ERROR) {
                layer->mSupportedMPPFlag |= mM2mMPPs[j]->mLogicalType;
                HDEBUGLOGD(eDebugResourceAssigning, "\t%s: supported", mM2mMPPs[j]->mName.c_str());
            } else {
                if (((-ret) == eMPPUnsupportedFormat) &&
                    ((ret = mM2mMPPs[j]->isSupported(display->mDisplayInfo, src_img, dst_img_yuv)) == NO_ERROR)) {
                    layer->mSupportedMPPFlag |= mM2mMPPs[j]->mLogicalType;
                    HDEBUGLOGD(eDebugResourceAssigning, "\t%s: supported with yuv dst", mM2mMPPs[j]->mName.c_str());
                }
            }
            if (ret < 0) {
                HDEBUGLOGD(eDebugResourceAssigning, "\t%s: unsupported flag(0x%" PRIx64 ")", mM2mMPPs[j]->mName.c_str(), -ret);
                uint64_t checkFlag = 0x0;
                if (layer->mCheckMPPFlag.find(mM2mMPPs[j]->mLogicalType) !=
                    layer->mCheckMPPFlag.end()) {
                    checkFlag = layer->mCheckMPPFlag.at(mM2mMPPs[j]->mLogicalType);
                }
                checkFlag |= (-ret);
                layer->mCheckMPPFlag[mM2mMPPs[j]->mLogicalType] = checkFlag;
            }
        }
        HDEBUGLOGD(eDebugResourceAssigning, "[%d] layer mSupportedMPPFlag(0x%8x)", i, layer->mSupportedMPPFlag);
    }
    HDEBUGLOGD(eDebugResourceAssigning, "%s-------------", __func__);

    return NO_ERROR;
}

int32_t ExynosResourceManager::resetResources() {
    HDEBUGLOGD(eDebugResourceManager, "%s+++++++++", __func__);

    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        mOtfMPPs[i]->resetMPP();
        mOtfMPPs[i]->clearVotfInfo();
        if (hwcCheckDebugMessages(eDebugResourceManager)) {
            String8 dumpMPP;
            mOtfMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        mM2mMPPs[i]->resetMPP();
        mM2mMPPs[i]->clearVotfInfo();
        if (hwcCheckDebugMessages(eDebugResourceManager)) {
            String8 dumpMPP;
            mM2mMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
    }

    HDEBUGLOGD(eDebugResourceManager, "%s-----------", __func__);
    return NO_ERROR;
}

uint32_t ExynosResourceManager::getExceptionScenarioFlag(ExynosMPP *mpp) {
    if (mpp->mEnableByDebug == false)
        return static_cast<uint32_t>(DisableType::DISABLE_DEBUG);
    else
        return static_cast<uint32_t>(DisableType::DISABLE_NONE);
}

int32_t ExynosResourceManager::checkExceptionScenario(uint64_t &geometryFlag) {
    auto checkDisabled = [&](ExynosMPPVector &mpps) {
        for (auto mpp : mpps) {
            uint32_t disableByUserScenario = 0;
            disableByUserScenario |= getExceptionScenarioFlag(mpp);
            if (mpp->mDisableByUserScenario != disableByUserScenario) {
                geometryFlag |= GEOMETRY_DEVICE_SCENARIO_CHANGED;
                mpp->mDisableByUserScenario = disableByUserScenario;
            }
        }
    };
    checkDisabled(mOtfMPPs);
    checkDisabled(mM2mMPPs);
    return NO_ERROR;
}

int32_t ExynosResourceManager::preAssignResources() {
    HDEBUGLOGD(eDebugResourceManager | eDebugResourceSetReserve, "%s+++++++++", __func__);
    uint32_t displayMode = mDeviceInfo.displayMode;

    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i]->mPreAssignDisplayList[displayMode] != 0) {
            HDEBUGLOGD(eDebugResourceAssigning, "\t%s check, dispMode(%d), 0x%8x", mOtfMPPs[i]->mName.c_str(), displayMode, mOtfMPPs[i]->mPreAssignDisplayList[displayMode]);

            ExynosDisplay *display = NULL;
            bool isReserved = false;
            for (size_t j = 0; j < mDisplays.size(); j++) {
                display = mDisplays[j];
                if (display == NULL || !display->mPlugState)
                    continue;
                int checkBit = mOtfMPPs[i]->mPreAssignDisplayList[displayMode] & display->getDisplayPreAssignBit();
                HDEBUGLOGD(eDebugResourceAssigning, "\t\tdisplay index(%zu), checkBit(%d)", j, checkBit);
                if (checkBit) {
                    HDEBUGLOGD(eDebugResourceAssigning, "\t\tdisplay index(%zu), displayId(%d), display(%p)", j, display->mDisplayId, display);
                    HDEBUGLOGD(eDebugResourceAssigning, "\t\treserve to display %d", display->mDisplayId);
                    mOtfMPPs[i]->reserveMPP(display->mDisplayInfo);
                    isReserved = true;
                    break;
                }
            }
            if (!isReserved) {
                uint32_t displayId = findAlternateDisplay(mOtfMPPs[i]);
                mOtfMPPs[i]->reserveMPP(getDisplay(displayId)->mDisplayInfo);
            }
        }
    }

    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        HDEBUGLOGD(eDebugResourceAssigning, "\t%s check, 0x%8x", mM2mMPPs[i]->mName.c_str(), mM2mMPPs[i]->mPreAssignDisplayList[displayMode]);
        if (mM2mMPPs[i]->mPreAssignDisplayList[displayMode] != 0) {
            ExynosDisplay *display = NULL;
            for (size_t j = 0; j < mDisplays.size(); j++) {
                display = mDisplays[j];
                if (display == NULL)
                    continue;
                int checkBit =
                    mM2mMPPs[i]->mPreAssignDisplayList[displayMode] & display->getDisplayPreAssignBit();
                HDEBUGLOGD(eDebugResourceAssigning, "\t\tdisplay index(%zu), checkBit(%d)", j, checkBit);
                if (checkBit) {
                    HDEBUGLOGD(eDebugResourceAssigning, "\t\tdisplay index(%zu), reserve displayId(%d), display(%p)",
                               j, display->mDisplayId, display);
                    mM2mMPPs[i]->reserveMPP(display->mDisplayInfo);
                    break;
                }
            }
        }
    }
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (hwcCheckDebugMessages(eDebugResourceManager)) {
            String8 dumpMPP;
            mOtfMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (hwcCheckDebugMessages(eDebugResourceManager)) {
            String8 dumpMPP;
            mM2mMPPs[i]->dump(dumpMPP);
            HDEBUGLOGD(eDebugResourceManager, "%s", dumpMPP.c_str());
        }
    }
    HDEBUGLOGD(eDebugResourceManager | eDebugResourceSetReserve, "%s-----------", __func__);
    return NO_ERROR;
}

ExynosMPP *ExynosResourceManager::getExynosMPP(uint32_t physicalType, uint32_t physicalIndex) {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if ((mOtfMPPs[i]->mPhysicalType == physicalType) &&
            (mOtfMPPs[i]->mPhysicalIndex == physicalIndex))
            return mOtfMPPs[i];
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if ((mM2mMPPs[i]->mPhysicalType == physicalType) &&
            (mM2mMPPs[i]->mPhysicalIndex == physicalIndex))
            return mM2mMPPs[i];
    }

    return NULL;
}

ExynosMPP *ExynosResourceManager::getExynosMPPForBlending(ExynosDisplay *display) {
    uint32_t displayMode = mDeviceInfo.displayMode;
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        ExynosMPP *mpp = mM2mMPPs[i];
        if (display == nullptr)
            return nullptr;

        if ((mpp->mMaxSrcLayerNum > 1) &&
            (mpp->mPreAssignDisplayList[displayMode] & display->getDisplayPreAssignBit()))
            return mpp;
    }

    return NULL;
}

int32_t ExynosResourceManager::updateResourceState() {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i]->mAssignedSources.size() == 0)
            mOtfMPPs[i]->requestHWStateChange(MPP_HW_STATE_IDLE);
        mOtfMPPs[i]->mPrevAssignedState = mOtfMPPs[i]->mAssignedState;
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mAssignedSources.size() == 0)
            mM2mMPPs[i]->requestHWStateChange(MPP_HW_STATE_IDLE);
        mM2mMPPs[i]->mPrevAssignedState = mM2mMPPs[i]->mAssignedState;
        mM2mMPPs[i]->mWasUsedPrevFrame = false;
    }
    return NO_ERROR;
}

/*
 * This function is called every frame.
 * This base function does nothing.
 * Module that supports setting frame rate should implement this function
 * in the module source code (hardware/samsung_slsi-linaro/graphics/exynos...).
 */
void ExynosResourceManager::setFrameRateForPerformance(ExynosMPP &mpp,
                                                       AcrylicPerformanceRequestFrame *frame) {
    int M2MFps = (int)(1000 / mpp.mCapacity);
    HDEBUGLOGD(eDebugResourceAssigning, "M2M(%d) setFrameRate %d", mpp.mPhysicalType, M2MFps);
    frame->setFrameRate(M2MFps);
}

int32_t ExynosResourceManager::deliverPerformanceInfo(ExynosDisplay *display) {
    int ret = NO_ERROR;

    if (display == nullptr)
        return -EINVAL;

    for (uint32_t mpp_physical_type = MPP_DPP_NUM; mpp_physical_type < MPP_P_TYPE_MAX; mpp_physical_type++) {
        AcrylicPerformanceRequest request;
        uint32_t assignedInstanceNum = 0;
        uint32_t assignedInstanceIndex = 0;
        ExynosMPP *mpp = NULL;
        bool canSkipSetting = true;

        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            mpp = mM2mMPPs[i];
            if (mpp->mPhysicalType != mpp_physical_type)
                continue;
            if (mpp->mAssignedDisplayInfo.displayIdentifier.id == UINT32_MAX) {
                continue;
            }
            /* Performance setting can be skipped
             * if all of instance's mPrevAssignedState, mAssignedState
             * are MPP_ASSIGN_STATE_FREE
             */
            if ((mpp->mPrevAssignedState & MPP_ASSIGN_STATE_ASSIGNED) ||
                (mpp->mAssignedState & MPP_ASSIGN_STATE_ASSIGNED)) {
                canSkipSetting = false;
            }

            /* post processing is not performed */
            if ((mpp->mAssignedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&
                mpp->canSkipProcessing())
                continue;
            /*
             * canSkipProcessing() can not be used if post processing
             * was already performed
             * becasue variables that are used in canSkipProcessing()
             * to check whether there was update or not are set when
             * post processing is perfomed.
             */
            if ((display->mRenderingStateFlags.validateFlag ||
                 display->mRenderingStateFlags.presentFlag) &&
                (mpp->mWasUsedPrevFrame == true)) {
                continue;
            }

            if (mpp->mAssignedSources.size() > 0) {
                assignedInstanceNum++;
            }
        }
        if ((canSkipSetting == true) && (assignedInstanceNum != 0))
            HWC_LOGE_NODISP("%s:: canSKip true but assignedInstanceNum(%d)",
                            __func__, assignedInstanceNum);

        if (canSkipSetting == true) {
            continue;
        }

        request.reset(assignedInstanceNum);

        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            mpp = mM2mMPPs[i];
            if ((mpp->mPhysicalType == mpp_physical_type) &&
                (mpp->mAssignedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
                (mpp->mAssignedSources.size() > 0)) {
                /* post processing is not performed */
                if ((mpp->mAssignedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&
                    mpp->canSkipProcessing())
                    continue;

                /*
                 * canSkipProcessing() can not be used if post processing
                 * was already performed
                 * becasue variables that are used in canSkipProcessing()
                 * to check whether there was update or not are set when
                 * post processing is perfomed.
                 */
                if ((display->mRenderingStateFlags.validateFlag ||
                     display->mRenderingStateFlags.presentFlag) &&
                    (mpp->mWasUsedPrevFrame == true))
                    continue;

                if (assignedInstanceIndex >= assignedInstanceNum) {
                    HWC_LOGE_NODISP("assignedInstanceIndex error (%d, %d)", assignedInstanceIndex, assignedInstanceNum);
                    break;
                }
                AcrylicPerformanceRequestFrame *frame = request.getFrame(assignedInstanceIndex);
                if (frame->reset(mpp->mAssignedSources.size()) == false) {
                    HWC_LOGE_NODISP("%d frame reset fail (%zu)", assignedInstanceIndex, mpp->mAssignedSources.size());
                    break;
                }
                setFrameRateForPerformance(*mpp, frame);

                for (uint32_t j = 0; j < mpp->mAssignedSources.size(); j++) {
                    ExynosMPPSource *mppSource = mpp->mAssignedSources[j];
                    frame->setSourceDimension(j,
                                              mppSource->mSrcImg.w, mppSource->mSrcImg.h,
                                              mppSource->mSrcImg.exynosFormat.halFormat());

                    if (mppSource->mSrcImg.compressionInfo.type != COMP_TYPE_NONE)
                        frame->setAttribute(j, AcrylicCanvas::ATTR_COMPRESSED);

                    hwc_rect_t src_area;
                    src_area.left = mppSource->mSrcImg.x;
                    src_area.top = mppSource->mSrcImg.y;
                    src_area.right = mppSource->mSrcImg.x + mppSource->mSrcImg.w;
                    src_area.bottom = mppSource->mSrcImg.y + mppSource->mSrcImg.h;

                    hwc_rect_t out_area;
                    out_area.left = mppSource->mMidImg.x;
                    out_area.top = mppSource->mMidImg.y;
                    out_area.right = mppSource->mMidImg.x + mppSource->mMidImg.w;
                    out_area.bottom = mppSource->mMidImg.y + mppSource->mMidImg.h;

                    frame->setTransfer(j, src_area, out_area, mppSource->mSrcImg.transform);
                }
                uint32_t format = mpp->mAssignedSources[0]->mMidImg.exynosFormat.halFormat();
                bool hasSolidColorLayer = false;
                if ((mpp->mMaxSrcLayerNum > 1) &&
                    (mpp->mAssignedSources.size() > 1) &&
                    (mpp->mNeedSolidColorLayer)) {
                    format = DEFAULT_MPP_DST_FORMAT;
                    hasSolidColorLayer = true;
                }

                frame->setTargetDimension(mpp->mAssignedDisplayInfo.xres,
                                          mpp->mAssignedDisplayInfo.yres, format, hasSolidColorLayer);

                assignedInstanceIndex++;
            }
        }
        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            mpp = mM2mMPPs[i];
            if ((display->mRenderingStateFlags.validateFlag ||
                 display->mRenderingStateFlags.presentFlag) &&
                (mpp->mWasUsedPrevFrame == true))
                continue;
            if ((mpp->mPhysicalType == mpp_physical_type) &&
                (mpp->mAssignedSources.size() > 0) &&
                (mpp->mAssignedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&
                (mpp->canSkipProcessing() == false)) {
                mpp->mAcrylicHandle->requestPerformanceQoS(&request);
                break;
            }
        }
    }
    return ret;
}

/*
 * Get used capacity of the resource that abstracts same HW resource
 * but it is different instance with mpp
 */
float ExynosResourceManager::getResourceUsedCapa(ExynosMPP &mpp) {
    float usedCapa = 0;
    if (mpp.mCapacity < 0)
        return usedCapa;

    HDEBUGLOGD(eDebugResourceAssigning, "%s:: [%s][%d] mpp[%d, %d]",
               __func__, mpp.mName.c_str(), mpp.mLogicalIndex,
               mpp.mPhysicalType, mpp.mPhysicalIndex);

    if (mpp.mMPPType == MPP_TYPE_OTF) {
        for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
            if ((mpp.mPhysicalType == mOtfMPPs[i]->mPhysicalType) &&
                (mpp.mPhysicalIndex == mOtfMPPs[i]->mPhysicalIndex)) {
                usedCapa += mOtfMPPs[i]->mUsedCapacity;
            }
        }
    } else {
        for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
            if ((mpp.mPhysicalType == mM2mMPPs[i]->mPhysicalType) &&
                (mpp.mPhysicalIndex == mM2mMPPs[i]->mPhysicalIndex)) {
                usedCapa += mM2mMPPs[i]->mUsedCapacity;
                usedCapa += mM2mMPPs[i]->mPreAssignedCapacity;
            }
        }
    }

    HDEBUGLOGD(eDebugResourceAssigning, "\t[%s][%d] mpp usedCapa: %f",
               mpp.mName.c_str(), mpp.mLogicalIndex, usedCapa);
    return usedCapa;
}

void ExynosResourceManager::enableMPP(uint32_t physicalType, uint32_t physicalIndex,
                                      uint32_t logicalIndex, uint32_t enable) {
    mEnableMPPRequests.emplace_back(EnableMPPRequest(physicalType, physicalIndex,
                                                     logicalIndex, enable));
}

bool ExynosResourceManager::applyEnableMPPRequests() {
    if (mEnableMPPRequests.size() == 0)
        return false;

    auto enableMPP = [&](ExynosMPPVector &mpps,
                         EnableMPPRequest &request) -> bool {
        for (auto &mpp : mpps) {
            if ((mpp->mPhysicalType == request.physicalType) &&
                (mpp->mPhysicalIndex == request.physicalIndex) &&
                (mpp->mLogicalIndex == request.logicalIndex)) {
                mpp->mEnableByDebug = !!(request.enable);
                return true;
            }
        }
        return false;
    };

    for (auto &it : mEnableMPPRequests) {
        if (enableMPP(mOtfMPPs, it))
            continue;
        enableMPP(mM2mMPPs, it);
    }

    mEnableMPPRequests.clear();
    return true;
}

int32_t ExynosResourceManager::prepareResources() {
    int ret = NO_ERROR;
    HDEBUGLOGD(eDebugResourceManager, "This is first validate");
    if ((ret = resetResources()) != NO_ERROR) {
        HWC_LOGE_NODISP("%s:: resetResources() error (%d)",
                        __func__, ret);
        return ret;
    }

    if ((ret = preAssignResources()) != NO_ERROR) {
        HWC_LOGE_NODISP("%s:: preAssignResources() error (%d)",
                        __func__, ret);
        return ret;
    }

    preAssignWindows();

    preAssignM2MResources();

    setDisplaysTDMInfo();

    /* It should be called after prepareResources
       because it needs that id value of MPPs->mReservedDisplayInfo is not null */
    setM2mTargetCompression();

    return ret;
}

int32_t ExynosResourceManager::finishAssignResourceWork() {
    int ret = NO_ERROR;
    if ((ret = updateResourceState()) != NO_ERROR) {
        HWC_LOGE_NODISP("%s:: stopUnAssignedResource() error (%d)",
                        __func__, ret);
        return ret;
    }

    return ret;
}

void ExynosResourceManager::makeM2MRestrictions() {
    uint32_t num_mpp_units = sizeof(AVAILABLE_M2M_MPP_UNITS) / sizeof(exynos_mpp_t);
    for (uint32_t i = MPP_DPP_NUM; i < MPP_P_TYPE_MAX; i++) {
        for (uint32_t j = 0; j < num_mpp_units; j++) {
            if (AVAILABLE_M2M_MPP_UNITS[j].physicalType == i) {
                makeAcrylRestrictions((mpp_phycal_type_t)i);
                break;
            }
        }
    }
}

void ExynosResourceManager::makeAcrylRestrictions(mpp_phycal_type_t type) {
    Acrylic *arc = NULL;
    if (type == MPP_MSC)
        arc = Acrylic::createScaler();
    else if (type == MPP_G2D)
        arc = Acrylic::createCompositor();
    else {
        ALOGE("Unknown MPP");
        return;
    }

    const HW2DCapability *cap;
    cap = &arc->getCapabilities();

    if (type == MPP_MSC) {
        const std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> &ppcTable = arc->getTablePPC();
        if (!ppcTable.empty()) {
            HDEBUGLOGD(eDebugAttrSetting, "MSCL ppc table++++");
            ppc_table_map.clear();
            for (auto iter = ppcTable.begin(); iter != ppcTable.end(); iter++) {
                uint32_t hal_format, ppc_format;
                hal_format = std::get<0>(*iter);
                if (isFormatSBWC(hal_format))
                    ppc_format = PPC_FORMAT_SBWC;
                else if (isFormatP010(hal_format))
                    ppc_format = PPC_FORMAT_P010;
                else if (isFormatYUV8_2(hal_format))
                    ppc_format = PPC_FORMAT_YUV8_2;
                else if (isFormatYUV420(hal_format))
                    ppc_format = PPC_FORMAT_YUV420;
                else if (isFormatYUV422(hal_format))
                    ppc_format = PPC_FORMAT_YUV422;
                else
                    ppc_format = PPC_FORMAT_RGB32;
                float ppc_rot_no = (float)std::get<1>(*iter) / 100;
                float ppc_rot = (float)std::get<2>(*iter) / 100;

                ppc_table_map[PPC_IDX(MPP_MSC, ppc_format, PPC_ROT_NO)].ppcList[0] = ppc_rot_no;
                ppc_table_map[PPC_IDX(MPP_MSC, ppc_format, PPC_ROT)].ppcList[0] = ppc_rot;
                HDEBUGLOGD(eDebugAttrSetting, "ppc_format: %u rot: %f rot no: %f",
                           ppc_format, ppc_rot_no, ppc_rot);
            }
            HDEBUGLOGD(eDebugAttrSetting, "MSCL ppc table----");

            for (auto mpp : mM2mMPPs) {
                if (mpp->mPhysicalType == MPP_MSC) {
                    mpp->updatePPCTable(ppc_table_map);
                    break;
                }
            }

        } else {
            HDEBUGLOGD(eDebugAttrSetting, "ppc info is written in source code, please check code");
        }
    }

    /* format restriction */
    std::unordered_set<int32_t> supportedHalFormats;
    for (uint32_t i = 0; i < FORMAT_MAX_CNT; i++) {
        if (cap->isFormatSupported(exynos_format_desc[i].halFormat)) {
            /* Not add same hal pixel format */
            if (supportedHalFormats.count(exynos_format_desc[i].halFormat))
                continue;
            restriction_key_t queried_format;
            queried_format.hwType = type;
            queried_format.nodeType = NODE_NONE;
            queried_format.format = exynos_format_desc[i].halFormat;
            queried_format.reserved = 0;
            addFormatRestrictions(queried_format);
            supportedHalFormats.insert(exynos_format_desc[i].halFormat);
        }
    }

    /* RGB size restrictions */
    restriction_size rSize;
    rSize.maxDownScale = cap->supportedMinMinification().hori;
    rSize.maxUpScale = cap->supportedMaxMagnification().hori;
    rSize.maxFullWidth = cap->supportedMaxSrcDimension().hori;
    rSize.maxFullHeight = cap->supportedMaxSrcDimension().vert;
    rSize.minFullWidth = cap->supportedMinSrcDimension().hori;
    rSize.minFullHeight = cap->supportedMinSrcDimension().vert;
    rSize.fullWidthAlign = cap->supportedDimensionAlign().hori;
    rSize.fullHeightAlign = cap->supportedDimensionAlign().vert;
    rSize.maxCropWidth = cap->supportedMaxSrcDimension().hori;
    rSize.maxCropHeight = cap->supportedMaxSrcDimension().vert;
    rSize.minCropWidth = cap->supportedMinSrcDimension().hori;
    rSize.minCropHeight = cap->supportedMinSrcDimension().vert;
    rSize.cropXAlign = cap->supportedDimensionAlign().hori;
    rSize.cropYAlign = cap->supportedDimensionAlign().vert;
    rSize.cropWidthAlign = cap->supportedDimensionAlign().hori;
    rSize.cropHeightAlign = cap->supportedDimensionAlign().vert;

    addSizeRestrictions(type, rSize, rSize, RESTRICTION_RGB);

    /* YUV size restrictions */
    rSize.fullWidthAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().hori),
                               YUV_CHROMA_H_SUBSAMPLE);
    rSize.fullHeightAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().vert),
                                YUV_CHROMA_V_SUBSAMPLE);
    rSize.cropXAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().hori),
                           YUV_CHROMA_H_SUBSAMPLE);
    rSize.cropYAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().vert),
                           YUV_CHROMA_V_SUBSAMPLE);
    rSize.cropWidthAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().hori),
                               YUV_CHROMA_H_SUBSAMPLE);
    rSize.cropHeightAlign = max(static_cast<uint32_t>(cap->supportedDimensionAlign().vert),
                                YUV_CHROMA_V_SUBSAMPLE);

    addSizeRestrictions(type, rSize, rSize, RESTRICTION_YUV);

    delete arc;
}

mpp_phycal_type_t ExynosResourceManager::getPhysicalType(int ch) const {
    for (auto mpp : mOtfMPPs) {
        if (mpp->mChId == ch)
            return static_cast<mpp_phycal_type_t>(mpp->mPhysicalType);
    }
    return MPP_P_TYPE_MAX;
}

ExynosMPP *ExynosResourceManager::getOtfMPPWithChannel(int ch) {
    for (auto mpp : mOtfMPPs) {
        if (mpp->mChId == ch)
            return mpp;
    }
    return nullptr;
}

void ExynosResourceManager::updateRestrictions() {
    uint32_t formatRestrictionCnt = sizeof(restriction_format_table) / sizeof(restriction_key);
    for (uint32_t i = 0; i < formatRestrictionCnt; i++)
        addFormatRestrictions(restriction_format_table[i]);

    // i = RGB, YUV
    // j = Size restriction count for each format (YUV, RGB)
    for (uint32_t i = 0; i < sizeof(restriction_tables) / sizeof(restriction_table_element); i++) {
        int size = restriction_tables[i].table_element_size;
        for (uint32_t j = 0; j < size; j++) {
            const restriction_size_element &cur = restriction_tables[i].table[j];
            if (cur.key.nodeType == NODE_DST)
                continue;

            int32_t dstIndex = -1;
            if (cur.key.nodeType == NODE_NONE) {
                /* src, dst size restriction are same if nodeType is NODE_NONE */
                dstIndex = j;
            } else {
                /* Find dst size restriction */
                const restriction_key dstKey{cur.key.hwType, NODE_DST,
                                             cur.key.format, cur.key.reserved};
                for (int k = 0; k < size; k++) {
                    const restriction_size_element &dst = restriction_tables[i].table[k];
                    if (dst.key == dstKey)
                        dstIndex = k;
                }
            }
            if (dstIndex > 0) {
                addSizeRestrictions(cur.key.hwType,
                                    cur.sizeRestriction,
                                    restriction_tables[i].table[dstIndex].sizeRestriction,
                                    static_cast<restriction_classification>(i));
            } else {
                HWC_LOGE_NODISP("%s:: There is no dst restriction for mpp(%d)",
                                __func__, cur.key.hwType);
            }
        }
    }
}

uint32_t ExynosResourceManager::getFeatureTableSize() const {
    return sizeof(feature_table) / sizeof(feature_support_t);
}

bool ExynosResourceManager::checkLayerAttribute(ExynosDisplay *display, ExynosLayer *layer,
                                                size_t type) {
    exynos_image src_img;
    layer->setSrcExynosImage(&src_img);

    if (type == ATTRIBUTE_DRM)
        return layer->isDrm();
    if (type == ATTRIBUTE_HDR10PLUS)
        return hasHdr10Plus(src_img);
    if (type == ATTRIBUTE_HDR10)
        return hasHdrInfo(layer->mDataSpace);

    return false;
}

bool ExynosResourceManager::isProcessingWithInternals(ExynosDisplay *display,
                                                      exynos_image *src_img, exynos_image *dst_img) {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if ((mOtfMPPs[i]->mReservedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&
            (mOtfMPPs[i]->isSupported(display->mDisplayInfo, *src_img, *dst_img) == NO_ERROR))
            return true;
    }
    return false;
}

int ExynosResourceManager::setPreAssignedCapacity(ExynosDisplay *display, ExynosLayer *layer,
                                                  exynos_image *src_img, exynos_image *dst_img, mpp_phycal_type_t type) {
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if ((mM2mMPPs[i]->mReservedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&  // ID check
            (mM2mMPPs[i]->mPhysicalType == type) &&                                             // type check
            (isAssignable(mM2mMPPs[i], display, *src_img, *dst_img, layer))) {                  // capacity check

            exynos_image otf_dst_img = *dst_img;

            std::vector<exynos_image> image_lists;
            if (getCandidateM2mMPPOutImages(display, layer, image_lists) < 0)
                return -1;

            for (auto &otf_src_img : image_lists) {
                exynos_image m2m_src_img = *src_img;
                otf_src_img.transform = 0;
                otf_dst_img.transform = 0;
                /*
                 * This is the case that layer color transform should be
                 * addressed by otfMPP not m2mMPP
                 */
                if (otf_src_img.needColorTransform)
                    m2m_src_img.needColorTransform = false;

                int64_t ret = mM2mMPPs[i]->isSupported(display->mDisplayInfo, m2m_src_img, otf_src_img);

                if (ret != NO_ERROR)
                    continue;

                for (uint32_t j = 0; j < mOtfMPPs.size(); j++) {
                    if ((mOtfMPPs[j]->mReservedDisplayInfo.displayIdentifier.id == display->mDisplayId) &&
                        (mOtfMPPs[j]->isSupported(display->mDisplayInfo, otf_src_img, otf_dst_img))) {
                        mM2mMPPs[i]->mPreAssignedCapacity = mM2mMPPs[i]->getRequiredCapacity(display->mDisplayInfo, *src_img, *dst_img);
                        HDEBUGLOGD(eDebugResourceManager, "%s::[display %u] [MPP %s] preAssigned capacity : %lf",
                                   __func__, display->mDisplayId, mM2mMPPs[i]->mName.c_str(), mM2mMPPs[i]->mPreAssignedCapacity);
                        return 0;
                    }
                }
            }
        }
    }
    HDEBUGLOGD(eDebugResourceManager, "%s::Can not preassign capacity", __func__);
    return -1;
}

void ExynosResourceManager::preAssignM2MResources() {
    for (size_t i = 0; i < mM2mMPPs.size(); i++)
        mM2mMPPs[i]->mPreAssignedCapacity = (float)0.0f;

    auto setPreAssigning = [&](ExynosDisplay *display, uint32_t attribute) {
        for (size_t j = 0; j < display->mLayers.size(); j++) {
            if (checkLayerAttribute(display, display->mLayers[j], attribute)) {
                int ret = 0;
                exynos_image src_img, dst_img;
                display->mLayers[j]->setSrcExynosImage(&src_img);
                display->mLayers[j]->setDstExynosImage(&dst_img);

                if (!isProcessingWithInternals(display, &src_img, &dst_img)) {
                    ret = setPreAssignedCapacity(display, display->mLayers[j], &src_img, &dst_img, MPP_MSC);
                    if (ret)
                        ret = setPreAssignedCapacity(display, display->mLayers[j], &src_img, &dst_img, MPP_G2D);
                }
            }
        }
    };
    for (auto attribute : mLayerAttributePriority) {
        for (int32_t i = mDisplays.size() - 1; i >= 0; i--) {
            ExynosDisplay *display = mDisplays[i];
            if ((display == nullptr) || (display->mPlugState == false) ||
                (display->mType == HWC_DISPLAY_VIRTUAL))
                continue;
            setPreAssigning(display, attribute);
        }
    }
}

float ExynosResourceManager::getAssignedCapacity(uint32_t physicalType) {
    float totalCapacity = 0;

    for (size_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mPhysicalType == physicalType)
            totalCapacity += mM2mMPPs[i]->getAssignedCapacity();
    }
    return totalCapacity;
}

void ExynosResourceManager::setM2MCapa(uint32_t physicalType, uint32_t capa) {
    for (size_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mPhysicalType == physicalType)
            mM2mMPPs[i]->mCapacity = capa;
    }
}

float ExynosResourceManager::getM2MCapa(uint32_t physicalType) {
    float ret = 0;
    for (size_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mPhysicalType == physicalType)
            return mM2mMPPs[i]->mCapacity;
    }

    return ret;
}

bool ExynosResourceManager::hasHDR10PlusMPP() {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i] == NULL)
            continue;
        if (mOtfMPPs[i]->mAttr & MPP_ATTR_HDR10PLUS)
            return true;
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i] == NULL)
            continue;
        if (mM2mMPPs[i]->mAttr & MPP_ATTR_HDR10PLUS)
            return true;
    }

    return false;
}

ExynosMPP *ExynosResourceManager::getHDR10OtfMPP() {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i] == NULL)
            continue;
        if (mOtfMPPs[i]->mAttr & MPP_ATTR_HDR10)
            return mOtfMPPs[i];
    }
    return NULL;
}

void ExynosResourceManager::setM2mTargetCompression() {
    for (size_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i]->mLogicalType == MPP_LOGICAL_G2D_RGB) {
            mM2mMPPs[i]->mTargetCompressionInfo.type = COMP_TYPE_NONE;
            if (mM2mMPPs[i]->mReservedDisplayInfo.displayIdentifier.id == UINT32_MAX) {
                continue;
            } else {
                ExynosDisplay *display = getDisplay(mM2mMPPs[i]->mReservedDisplayInfo.displayIdentifier.id);
                if (display->mExynosCompositionInfo.mCompressionInfo.type == COMP_TYPE_AFBC)
                    mM2mMPPs[i]->mTargetCompressionInfo.type = COMP_TYPE_AFBC;
            }
        }
    }
}

void ExynosResourceManager::processHdr10plusToHdr10(ExynosDisplay *display) {
    int hdr10plus_mpp_cnt;
    int hdr10plus_layer_cnt;

    /* find mpps that can process hdr10plus */
    hdr10plus_mpp_cnt = 0;
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i] == NULL)
            continue;

        if ((mOtfMPPs[i]->mReservedDisplayInfo.displayIdentifier.id != UINT32_MAX) &&
            (mOtfMPPs[i]->mReservedDisplayInfo.displayIdentifier.id != display->mDisplayId)) {
            ExynosDisplay *tmpDisplay = getDisplay(mOtfMPPs[i]->mReservedDisplayInfo.displayIdentifier.id);
            if (tmpDisplay && tmpDisplay->mPlugState == true)
                continue;
        }

        if (mOtfMPPs[i]->mAttr & MPP_ATTR_HDR10PLUS)
            hdr10plus_mpp_cnt++;
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i] == NULL)
            continue;

        if (mM2mMPPs[i]->mReservedDisplayInfo.displayIdentifier.id != display->mDisplayId)
            continue;

        if (mM2mMPPs[i]->mAttr & MPP_ATTR_HDR10PLUS)
            hdr10plus_mpp_cnt++;
    }

    hdr10plus_layer_cnt = 0;
    for (int32_t i = 0; i < display->mLayers.size(); i++) {
        ExynosLayer *layer = display->mLayers[i];

        if (layer->mIsHdr10PlusLayer)
            hdr10plus_layer_cnt++;
        else
            continue;

        exynos_image src_img;
        exynos_image dst_img;
        layer->setSrcExynosImage(&src_img);
        /* if hdr10plus mpps are not enough, clear layer's dynamic metatype */
        if (hdr10plus_layer_cnt > hdr10plus_mpp_cnt) {
            if (hasHdr10Plus(src_img)) {
                if (layer->clearHdrDynamicType() == NO_ERROR) {
                    layer->setSrcExynosImage(&src_img);
                    layer->setDstExynosImage(&dst_img);
                    layer->setExynosImage(src_img, dst_img);
                    HDEBUGLOGD(eDebugLayer,
                               "%s::[%d] layer's hdr10p info is removed", __func__, i);
                }
            }
        }
        /* if metatype of hdr10plus is cleard, but can be processed, restore layer's dynamic metatype */
        else {
            if (!hasHdr10Plus(src_img)) {
                if (layer->restoreHdrDynamicType() == NO_ERROR) {
                    layer->setSrcExynosImage(&src_img);
                    layer->setDstExynosImage(&dst_img);
                    layer->setExynosImage(src_img, dst_img);
                    HDEBUGLOGD(eDebugLayer,
                               "%s::[%d] layer's hdr10p info is restored", __func__, i);
                }
            }
        }
    }
}

void ExynosResourceManager::updateSupportWCG() {
    for (uint32_t i = 0; i < mOtfMPPs.size(); i++) {
        if (mOtfMPPs[i] == NULL)
            continue;
        if (mOtfMPPs[i]->mAttr & (MPP_ATTR_WCG | MPP_ATTR_HDR10))
            mDeviceSupportWCG = true;
    }
    for (uint32_t i = 0; i < mM2mMPPs.size(); i++) {
        if (mM2mMPPs[i] == NULL)
            continue;
        if (mM2mMPPs[i]->mAttr & (MPP_ATTR_WCG | MPP_ATTR_HDR10))
            mDeviceSupportWCG = true;
    }
}

void ExynosResourceManager::makeDPURestrictions(
    struct dpp_restrictions_info_v2 *dpuInfo, bool checkOverlap) {
    bool overlap[16] = {
        false,
    };

    HDEBUGLOGD(eDebugAttrSetting, "DPP ver : %d, cnt : %d", dpuInfo->ver, dpuInfo->dpp_cnt);

    /* format resctriction */
    for (int i = 0; i < dpuInfo->dpp_cnt; i++) {
        dpp_restriction r = dpuInfo->dpp_ch[i].restriction;
        HDEBUGLOGD(eDebugAttrSetting, "id : %d, format count : %d", i, r.format_cnt);
    }

    /* Check attribute overlap */
    if (checkOverlap) {
        std::unordered_set<unsigned long> attrs;
        for (size_t i = 0; i < dpuInfo->dpp_cnt; ++i) {
            const dpp_ch_restriction &r = dpuInfo->dpp_ch[i];
            if (attrs.find(r.attr) != attrs.end())
                overlap[i] = true;
            else
                attrs.insert(r.attr);
            HDEBUGLOGD(eDebugAttrSetting, "Index : %zu, overlap %d", i, overlap[i]);
        }
    }

    for (int i = 0; i < dpuInfo->dpp_cnt; i++) {
        if (checkOverlap && overlap[i])
            continue;
        dpp_restriction r = dpuInfo->dpp_ch[i].restriction;
        mpp_phycal_type_t hwType = getPhysicalType(i);
        ExynosMPP *mpp = getOtfMPPWithChannel(i);
        if (mpp == nullptr)
            continue;
        for (int j = 0; j < r.format_cnt; j++) {
            restriction_key_t queried_format;
            queried_format.hwType = hwType;
            queried_format.nodeType = NODE_NONE;
            /* r.format[j] is HAL format */
            queried_format.format = r.format[j];
            queried_format.reserved = 0;
            if (checkOverlap) {
                /* Add formats to the all mpps that have hwType */
                addFormatRestrictions(queried_format);
            } else {
                /*
                 * Add formats to the only the mpp that is mapped with
                 * the channel
                 */
                mpp->addFormatRestrictions(queried_format);
            }
            HDEBUGLOGD(eDebugAttrSetting, "%s : %d", mpp->mName.c_str(), r.format[j]);
        }
    }

    for (int i = 0; i < dpuInfo->dpp_cnt; i++) {
        if (checkOverlap && overlap[i])
            continue;
        const dpp_restriction &r = dpuInfo->dpp_ch[i].restriction;

        /* RGB size restrictions */
        restriction_size rSrcSize;
        restriction_size rDstSize;
        convertSizeRestriction(r, rSrcSize, rDstSize);

        mpp_phycal_type_t hwType = MPP_P_TYPE_MAX;
        ExynosMPP *mpp = nullptr;
        if (checkOverlap) {
            hwType = getPhysicalType(i);
            /* Set restrictions to the all mpps that have hwType */
            addSizeRestrictions(hwType, rSrcSize, rDstSize, RESTRICTION_RGB);
        } else {
            mpp = getOtfMPPWithChannel(i);
            if (mpp) {
                /*
                 * Set restrictions to the only the mpp that is mapped with
                 * the channel
                 */
                mpp->addSizeRestrictions(rSrcSize, rDstSize, RESTRICTION_RGB);
            } else {
                HWC_LOGE_NODISP("%s:: otfMPP is null for dpp channel(%d)",
                                __func__, i);
            }
        }

        /* YUV size restrictions */
        convertYUVSizeRestriction(r, rSrcSize, rDstSize);

        if (checkOverlap) {
            addSizeRestrictions(hwType, rSrcSize, rDstSize, RESTRICTION_YUV);
        } else {
            mpp->addSizeRestrictions(rSrcSize, rDstSize, RESTRICTION_YUV);
            /* update otfMPP featurs */
            setDPUFeature(mpp, dpuInfo->dpp_ch[i].attr);
        }
    }
}

void ExynosResourceManager::updateMPPFeature(bool updateOtfMPP) {
    for (auto &feature : feature_table) {
        for (auto &m2mMPP : mM2mMPPs) {
            if (m2mMPP->mPhysicalType == feature.hwType) {
                m2mMPP->mAttr = feature.attr;
                HDEBUGLOGD(eDebugAttrSetting, "Attr:%s:  0x%" PRIx64 "",
                           m2mMPP->mName.c_str(), feature.attr);
            }
        }

        /*
         * otfMPP feature was updated by makeDPURestrictions
         */
        if (!updateOtfMPP)
            continue;

        for (auto &otfMPP : mOtfMPPs) {
            if (otfMPP->mPhysicalType == feature.hwType) {
                otfMPP->mAttr = feature.attr;
                HDEBUGLOGD(eDebugAttrSetting, "Attr:%s:  0x%" PRIx64 "",
                           otfMPP->mName.c_str(), feature.attr);
            }
        }
    }
}

void ExynosResourceManager::updateFeatureTable(struct dpp_restrictions_info_v2 *dpuInfo) {
    bool overlap[16] = {
        false,
    };
    /* Check attribute overlap */
    std::unordered_set<unsigned long> attrs;
    for (size_t i = 0; i < dpuInfo->dpp_cnt; ++i) {
        const dpp_ch_restriction &r = dpuInfo->dpp_ch[i];
        if (attrs.find(r.attr) != attrs.end())
            overlap[i] = true;
        else
            attrs.insert(r.attr);
        HDEBUGLOGD(eDebugAttrSetting, "Index : %zu, overlap %d", i, overlap[i]);
    }
    const uint32_t featureTableCnt = getFeatureTableSize();
    const int attrMapCnt = sizeof(dpu_attr_map_table) / sizeof(dpu_attr_map_t);

    HDEBUGLOGD(eDebugAttrSetting, "Before");
    for (uint32_t j = 0; j < featureTableCnt; j++) {
        HDEBUGLOGD(eDebugAttrSetting, "type : %d, feature : 0x%lx",
                   feature_table[j].hwType,
                   (unsigned long)feature_table[j].attr);
    }

    HDEBUGLOGD(eDebugAttrSetting, "After");
    // dpp count
    for (int i = 0; i < dpuInfo->dpp_cnt; i++) {
        dpp_ch_restriction c_r = dpuInfo->dpp_ch[i];
        if (overlap[i])
            continue;
        HDEBUGLOGD(eDebugAttrSetting, "DPU attr : (ch:%d), 0x%lx", i, (unsigned long)c_r.attr);
        mpp_phycal_type_t hwType = getPhysicalType(i);
        // feature table count
        for (uint32_t j = 0; j < featureTableCnt; j++) {
            if (feature_table[j].hwType == hwType) {
                uint64_t attr = 0;
                // dpp attr count
                for (int k = 0; k < attrMapCnt; k++) {
                    if (c_r.attr & (1 << dpu_attr_map_table[k].dpp_attr)) {
                        attr |= dpu_attr_map_table[k].hwc_attr;
                    }
                }
                auto it = sw_feature_table.find(hwType);
                if (it != sw_feature_table.end())
                    attr |= it->second;
                feature_table[j].attr = attr;
                HDEBUGLOGD(eDebugAttrSetting, "type : %d, feature : 0x%lx",
                           feature_table[j].hwType,
                           (unsigned long)feature_table[j].attr);
            }
        }
    }
}

void ExynosResourceManager::setDPUFeature(ExynosMPP *mpp, uint64_t dpuAttr) {
    const int attrMapCnt = sizeof(dpu_attr_map_table) / sizeof(dpu_attr_map_t);
    uint64_t attr = 0;

    for (int i = 0; i < attrMapCnt; i++) {
        if (dpuAttr & (1 << dpu_attr_map_table[i].dpp_attr)) {
            attr |= dpu_attr_map_table[i].hwc_attr;
        }
    }

    mpp_phycal_type_t hwType = static_cast<mpp_phycal_type_t>(mpp->mPhysicalType);
    auto it = sw_feature_table.find(hwType);
    if (it != sw_feature_table.end())
        attr |= it->second;

    mpp->mAttr = attr;
    HDEBUGLOGD(eDebugAttrSetting, "Attr:%s: 0x%" PRIx64 "",
               mpp->mName.c_str(), attr);
}

void ExynosResourceManager::addFormatRestrictions(restriction_key_t restrictionNode) {
    auto addResriction = [&](ExynosMPPVector &mppList) {
        for (auto mpp : mppList) {
            if (restrictionNode.hwType == mpp->mPhysicalType)
                mpp->addFormatRestrictions(restrictionNode);
        }
    };
    if (restrictionNode.hwType < MPP_DPP_NUM)
        addResriction(mOtfMPPs);
    else
        addResriction(mM2mMPPs);
}

void ExynosResourceManager::addSizeRestrictions(uint32_t mppId,
                                                restriction_size src_size,
                                                restriction_size dst_size,
                                                restriction_classification format) {
    auto addResriction = [&](ExynosMPPVector &mppList) {
        for (auto mpp : mppList) {
            if (static_cast<mpp_phycal_type_t>(mppId) == mpp->mPhysicalType)
                mpp->addSizeRestrictions(src_size, dst_size, format);
        }
    };
    if (mppId < MPP_DPP_NUM)
        addResriction(mOtfMPPs);
    else
        addResriction(mM2mMPPs);
}

bool ExynosResourceManager::isAssignable(ExynosMPP *candidateMPP, ExynosDisplay *display,
                                         struct exynos_image &src, struct exynos_image &dst, ExynosMPPSource *mppSrc) {
    bool ret = true;

    float totalUsedCapacity = getResourceUsedCapa(*candidateMPP);
    ret = candidateMPP->isAssignable(display->mDisplayInfo, src, dst, totalUsedCapacity);

    if ((ret) && (mppSrc != nullptr)) {
        if ((candidateMPP->mMPPType == MPP_TYPE_OTF) &&
            (!isHWResourceAvailable(display, candidateMPP, mppSrc))) {
            if (mppSrc->mSourceType == MPP_SOURCE_LAYER) {
                ExynosLayer *layer = (ExynosLayer *)mppSrc;
                layer->mCheckMPPFlag[candidateMPP->mLogicalType] = eMPPExeedHWResource;
            }
            ret = false;
        }
    }

    return ret;
}

void ExynosResourceManager::checkAttrMPP(ExynosDisplay *display) {
    if (display == nullptr)
        return;

    auto checkHdrAttr = [&](ExynosMPPVector &mpps) {
        for (auto mpp : mpps) {
            if (mpp->mPreAssignDisplayInfo & (1 << display->mType)) {
                if (mpp->mAttr & MPP_ATTR_HDR10)
                    display->mHasHdr10AttrMPP = true;
                if (mpp->mAttr & MPP_ATTR_HDR10PLUS)
                    display->mHasHdr10PlusAttrMPP = true;
            }
        }
    };

    checkHdrAttr(mOtfMPPs);
    checkHdrAttr(mM2mMPPs);
}

int32_t ExynosResourceManager::printMppsAttr() {
    auto printRestrictions = [&](ExynosMPPVector &mppList) {
        for (auto mpp : mppList) {
            mpp->printMppsAttr();
        }
    };
    printRestrictions(mOtfMPPs);
    printRestrictions(mM2mMPPs);
    return NO_ERROR;
}

uint32_t ExynosResourceManager::needHWResource(ExynosDisplay *display, exynos_image &srcImg, exynos_image &dstImg, tdm_attr_t attr) {
    uint32_t ret = 0;

    switch (attr) {
    case TDM_ATTR_SBWC:
        ret = (srcImg.exynosFormat.isSBWC()) ? 1 : 0;
        break;
    case TDM_ATTR_SAJC:
        ret = (srcImg.compressionInfo.type == COMP_TYPE_SAJC) ? 1 : 0;
        break;
    case TDM_ATTR_ITP:
        ret = (srcImg.exynosFormat.isYUV()) ? 1 : 0;
        break;
    case TDM_ATTR_WCG:
        ret = (needHdrProcessing(display, srcImg, dstImg)) ? 1 : 0;
        HDEBUGLOGD(eDebugTDM, "needHdrProcessing : %d", ret);
        break;
    case TDM_ATTR_ROT_90:
        ret = ((srcImg.transform & HAL_TRANSFORM_ROT_90) == 0) ? 0 : 1;
        break;
    case TDM_ATTR_SCALE: {
        bool isPerpendicular = !!(srcImg.transform & HAL_TRANSFORM_ROT_90);
        if (isPerpendicular) {
            ret = ((srcImg.w != dstImg.h) || (srcImg.h != dstImg.w)) ? 1 : 0;
        } else {
            ret = ((srcImg.w != dstImg.w) || (srcImg.h != dstImg.h)) ? 1 : 0;
        }
    } break;
    default:
        ret = 0;
        break;
    }

    return ret;
}

void ExynosResourceManager::refineTransfer(uint32_t &ids) {
    int transfer = ids & HAL_DATASPACE_TRANSFER_MASK;
    int result = ids;
    switch (transfer) {
    case HAL_DATASPACE_TRANSFER_SRGB:
    case HAL_DATASPACE_TRANSFER_LINEAR:
    case HAL_DATASPACE_TRANSFER_ST2084:
    case HAL_DATASPACE_TRANSFER_HLG:
        break;
    default:
        result = result & ~(HAL_DATASPACE_TRANSFER_MASK);
        ids = (android_dataspace)(result | HAL_DATASPACE_TRANSFER_SRGB);
        break;
    }
}

bool ExynosResourceManager::needHdrProcessing(ExynosDisplay *display, exynos_image &srcImg, exynos_image &dstImg) {
    if (!deviceSupportWCG())
        return false;

    if (!display->isSupportLibHdrApi())
        return needHdrProcessingInternal(display, srcImg, dstImg);

    // Use libHdr Api //
    return display->needHdrProcessing(srcImg, dstImg);
}

bool ExynosResourceManager::needHdrProcessingInternal(ExynosDisplay *display, exynos_image &srcImg, exynos_image &dstImg) {
    uint32_t srcDataSpace = srcImg.dataSpace;
    uint32_t dstDataSpace = dstImg.dataSpace;

    refineTransfer(srcDataSpace);

    dstDataSpace = getRefinedDataspace(srcImg.exynosFormat.halFormat(), srcImg.dataSpace);

    /* By reference Layer's dataspace */
    uint32_t srcStandard = (srcDataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t srcTransfer = (srcDataSpace & HAL_DATASPACE_TRANSFER_MASK);
    uint32_t dstStandard = (dstDataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t dstTransfer = (dstDataSpace & HAL_DATASPACE_TRANSFER_MASK);

    /* Even if src's and dst's dataspace are same,
     * HDR(WCG) engine is used if the HDR dataspace is satisfied. */
    if (((srcStandard == HAL_DATASPACE_STANDARD_BT2020) ||
         (srcStandard == HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE)) &&
        ((srcTransfer == HAL_DATASPACE_TRANSFER_ST2084) || (srcTransfer == HAL_DATASPACE_TRANSFER_HLG)))
        return true;

    if ((srcStandard == dstStandard) && (srcTransfer == dstTransfer))
        return false;

    return true;
}
