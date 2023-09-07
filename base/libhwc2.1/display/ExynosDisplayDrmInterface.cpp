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

#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
#include <sys/types.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <drm.h>
#include <drm/drm_mode.h>
#include <cutils/properties.h>
#include "ExynosDisplayDrmInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosGraphicBuffer.h"
#include "DrmDataType.h"

using namespace vendor::graphics;

using namespace std::chrono_literals;

constexpr float DISPLAY_LUMINANCE_UNIT = 10000;
constexpr float CONSTANT_FOR_DP_MIN_LUMINANCE = 255.0f * 255.0f / 100.0f;
constexpr auto nsecsPerSec = std::chrono::nanoseconds(1s).count();
constexpr bool kUseBufferCaching = true;

typedef struct _drmModeAtomicReqItem drmModeAtomicReqItem, *drmModeAtomicReqItemPtr;

struct _drmModeAtomicReqItem {
    uint32_t object_id;
    uint32_t property_id;
    uint64_t value;
};

struct _drmModeAtomicReq {
    uint32_t cursor;
    uint32_t size_items;
    drmModeAtomicReqItemPtr items;
};

extern struct exynos_hwc_control exynosHWCControl;
static const int32_t kUmPerInch = 25400;

void ExynosDisplayInterface::removeBuffer(const uint64_t &bufferId) {
    static FramebufferManager &fbManager = FramebufferManager::getInstance();
    fbManager.removeBuffer(bufferId);
}

ExynosDisplayDrmInterface::ExynosDisplayDrmInterface()
    : mDrmDevice(nullptr),
      mDrmCrtc(nullptr),
      mDrmConnector(nullptr) {
    mType = INTERFACE_TYPE_DRM;
    mDrmReq.init(this);
}

ExynosDisplayDrmInterface::~ExynosDisplayDrmInterface() {
    if (mDrmDevice != nullptr) {
        removeFbs(mDrmDevice->fd(), mOldFbIds);
        removeFbs(mDrmDevice->fd(), mFbIds);

        if (mActiveModeState.blob_id)
            mDrmDevice->DestroyPropertyBlob(mActiveModeState.blob_id);
        if (mActiveModeState.old_blob_id)
            mDrmDevice->DestroyPropertyBlob(mActiveModeState.old_blob_id);
        if (mDesiredModeState.blob_id)
            mDrmDevice->DestroyPropertyBlob(mDesiredModeState.blob_id);
        if (mDesiredModeState.old_blob_id)
            mDrmDevice->DestroyPropertyBlob(mDesiredModeState.old_blob_id);
        if (mPartialRegionState.blob_id)
            mDrmDevice->DestroyPropertyBlob(mPartialRegionState.blob_id);
    }
}

void ExynosDisplayDrmInterface::init(const DisplayIdentifier &display,
                                     void *deviceData, const size_t deviceDataSize) {
    mDisplayIdentifier = display;
    DeviceInterfaceData *data = (DeviceInterfaceData *)deviceData;
    if ((data == nullptr) || (deviceDataSize != sizeof(DeviceInterfaceData))) {
        ALOGE("device data is not valid, size(%zu, %zu)", deviceDataSize,
              sizeof(DeviceInterfaceData));
        assert(0);
        return;
    }
    mDrmDevice = data->drmDevice;
    exynos_display_t display_t;
    for (size_t i = 0; i < DISPLAY_COUNT; i++) {
        display_t = AVAILABLE_DISPLAY_UNITS[i];
        if ((display_t.index == mDisplayIdentifier.index) &&
            (display_t.type == mDisplayIdentifier.type)) {
            break;
        }
    }
    initDrmDevice(mDrmDevice, atoi(display_t.decon_node_name));
}

void ExynosDisplayDrmInterface::parseEnums(const DrmProperty &property,
                                           const std::vector<std::pair<uint32_t, const char *>> &enums,
                                           std::unordered_map<uint32_t, uint64_t> &out_enums) {
    uint64_t value;
    int ret;
    for (auto &e : enums) {
        std::tie(value, ret) = property.GetEnumValueWithName(e.second);
        if (ret == NO_ERROR)
            out_enums[e.first] = value;
        else
            ALOGE("Fail to find enum value with name %s", e.second);
    }
}

void ExynosDisplayDrmInterface::parseBlendEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> blendEnums = {
        {HWC2_BLEND_MODE_NONE, "None"},
        {HWC2_BLEND_MODE_PREMULTIPLIED, "Pre-multiplied"},
        {HWC2_BLEND_MODE_COVERAGE, "Coverage"},
    };

    ALOGD("Init blend enums");
    parseEnums(property, blendEnums, mBlendEnums);
    for (auto &e : mBlendEnums) {
        ALOGD("blend [hal: %d, drm: %" PRId64 "]", e.first, e.second);
    }
}

void ExynosDisplayDrmInterface::parseStandardEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> standardEnums = {
        {HAL_DATASPACE_STANDARD_UNSPECIFIED, "Unspecified"},
        {HAL_DATASPACE_STANDARD_BT709, "BT709"},
        {HAL_DATASPACE_STANDARD_BT601_625, "BT601_625"},
        {HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED, "BT601_625_UNADJUSTED"},
        {HAL_DATASPACE_STANDARD_BT601_525, "BT601_525"},
        {HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED, "BT601_525_UNADJUSTED"},
        {HAL_DATASPACE_STANDARD_BT2020, "BT2020"},
        {HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE, "BT2020_CONSTANT_LUMINANCE"},
        {HAL_DATASPACE_STANDARD_BT470M, "BT470M"},
        {HAL_DATASPACE_STANDARD_FILM, "FILM"},
        {HAL_DATASPACE_STANDARD_DCI_P3, "DCI-P3"},
        {HAL_DATASPACE_STANDARD_ADOBE_RGB, "Adobe RGB"},
    };

    ALOGD("Init standard enums");
    parseEnums(property, standardEnums, mStandardEnums);
    for (auto &e : mStandardEnums) {
        ALOGD("standard [hal: %d, drm: %" PRId64 "]",
              e.first >> HAL_DATASPACE_STANDARD_SHIFT, e.second);
    }
}

void ExynosDisplayDrmInterface::parseTransferEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> transferEnums = {
        {HAL_DATASPACE_TRANSFER_UNSPECIFIED, "Unspecified"},
        {HAL_DATASPACE_TRANSFER_LINEAR, "Linear"},
        {HAL_DATASPACE_TRANSFER_SRGB, "sRGB"},
        {HAL_DATASPACE_TRANSFER_SMPTE_170M, "SMPTE 170M"},
        {HAL_DATASPACE_TRANSFER_GAMMA2_2, "Gamma 2.2"},
        {HAL_DATASPACE_TRANSFER_GAMMA2_6, "Gamma 2.6"},
        {HAL_DATASPACE_TRANSFER_GAMMA2_8, "Gamma 2.8"},
        {HAL_DATASPACE_TRANSFER_ST2084, "ST2084"},
        {HAL_DATASPACE_TRANSFER_HLG, "HLG"},
    };

    ALOGD("Init transfer enums");
    parseEnums(property, transferEnums, mTransferEnums);
    for (auto &e : mTransferEnums) {
        ALOGD("transfer [hal: %d, drm: %" PRId64 "]",
              e.first >> HAL_DATASPACE_TRANSFER_SHIFT, e.second);
    }
}

void ExynosDisplayDrmInterface::parseRangeEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> rangeEnums = {
        {HAL_DATASPACE_RANGE_UNSPECIFIED, "Unspecified"},
        {HAL_DATASPACE_RANGE_FULL, "Full"},
        {HAL_DATASPACE_RANGE_LIMITED, "Limited"},
        {HAL_DATASPACE_RANGE_EXTENDED, "Extended"},
    };

    ALOGD("Init range enums");
    parseEnums(property, rangeEnums, mRangeEnums);
    for (auto &e : mRangeEnums) {
        ALOGD("range [hal: %d, drm: %" PRId64 "]",
              e.first >> HAL_DATASPACE_RANGE_SHIFT, e.second);
    }
}

void ExynosDisplayDrmInterface::parseColorModeEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> colorModeEnums = {
        {HAL_COLOR_MODE_NATIVE, "Native"},
        {HAL_COLOR_MODE_DCI_P3, "DCI-P3"},
        {HAL_COLOR_MODE_SRGB, "SRGB"},
    };

    ALOGD("Init color mode enums");
    parseEnums(property, colorModeEnums, mColorModeEnums);
    for (auto &e : mColorModeEnums) {
        ALOGD("Colormode [hal: %d, drm: %" PRId64 "]", e.first, e.second);
    }
}

void ExynosDisplayDrmInterface::parsePanelTypeEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> panelTypeEnums = {
        {DECON_VIDEO_MODE, "video_mode"},
        {DECON_MIPI_COMMAND_MODE, "command_mode"},
    };

    ALOGD("Init Panel Type enums");
    parseEnums(property, panelTypeEnums, mPanelTypeEnums);
    for (auto &e : mPanelTypeEnums) {
        ALOGD("PanelType [hal: %d, drm: %" PRId64 "]",
              e.first, e.second);
    }
}

void ExynosDisplayDrmInterface::parseVirtual8kEnums(const DrmProperty &property) {
    const std::vector<std::pair<uint32_t, const char *>> virtual8kEnums = {
        {EXYNOS_SPLIT_NONE, "No Split"},
        {EXYNOS_SPLIT_LEFT, "Split Left"},
        {EXYNOS_SPLIT_RIGHT, "Split Right"},
        {EXYNOS_SPLIT_TOP, "Split Top"},
        {EXYNOS_SPLIT_BOTTOM, "Split Bottom"},
    };

    ALOGD("Init virtual 8k enums");
    parseEnums(property, virtual8kEnums, mVirtual8kEnums);
    for (auto &e : mVirtual8kEnums) {
        ALOGD("PanelType [hal: %d, drm: %" PRId64 "]",
              e.first, e.second);
    }
}

void ExynosDisplayDrmInterface::initDrmDevice(DrmDevice *drmDevice, int drmDisplayId) {
    ALOGI("%s:: drmDisplayId(%d), type(%d)", __func__, drmDisplayId, mDisplayIdentifier.type);

    if ((mDrmDevice = drmDevice) == NULL) {
        ALOGE("drmDevice is NULL");
        return;
    }

    if (mDisplayIdentifier.type != HWC_DISPLAY_EXTERNAL)
        mWritebackInfo.init(mDrmDevice, drmDisplayId);

    if ((mDrmCrtc = mDrmDevice->GetCrtcForDisplay(drmDisplayId)) == NULL) {
        ALOGE("%s:: GetCrtcForDisplay is NULL", mDisplayIdentifier.name.c_str());
        return;
    }

    if (mDisplayIdentifier.type == HWC_DISPLAY_VIRTUAL) {
        if ((mDrmConnector = mWritebackInfo.getWritebackConnector()) == NULL) {
            ALOGE("%s:: getWritebackConnector is NULL", mDisplayIdentifier.name.c_str());
            return;
        }
    } else {
        if ((mDrmConnector = mDrmDevice->GetConnectorForDisplay(drmDisplayId)) == NULL) {
            ALOGE("%s:: GetConnectorForDisplay is NULL", mDisplayIdentifier.name.c_str());
            return;
        }
        mDrmVSyncWorker.Init(mDrmDevice, drmDisplayId);
        mDrmVSyncWorker.RegisterCallback(static_cast<VsyncCallback *>(this));
    }

    getLowPowerDrmModeModeInfo();

    if (!mDrmDevice->planes().empty()) {
        auto &plane = mDrmDevice->planes().front();
        parseBlendEnums(plane->blend_property());
        parseStandardEnums(plane->standard_property());
        parseTransferEnums(plane->transfer_property());
        parseRangeEnums(plane->range_property());
        parseVirtual8kEnums(plane->virtual8k_split_property());
    }

    if (mDisplayIdentifier.type != HWC_DISPLAY_VIRTUAL)
        chosePreferredConfig();

    parseColorModeEnums(mDrmCrtc->color_mode_property());
    /* TODO check this is needed */
    parsePanelTypeEnums(mDrmCrtc->operation_mode_property());

    if (mDrmCrtc->operation_mode_property().id() == 0) {
        ALOGD("%s: panel type property is not supported",
              mDisplayIdentifier.name.c_str());
    } else {
        int ret;
        uint64_t EnumId;
        std::tie(ret, EnumId) = mDrmCrtc->operation_mode_property().value();
        ALOGD("%s: panel type property enum ID %lu",
              mDisplayIdentifier.name.c_str(), (unsigned long)EnumId);
    }

    return;
}

void ExynosDisplayDrmInterface::Callback(
    int display, int64_t timestamp) {
    mVsyncHandler->handleVsync(timestamp);
}

int32_t ExynosDisplayDrmInterface::getLowPowerDrmModeModeInfo() {
    int ret;
    uint64_t blobId;

    std::tie(ret, blobId) = mDrmConnector->lp_mode().value();
    if (ret) {
        ALOGE("Fail to get blob id for lp mode");
    }
    drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(mDrmDevice->fd(), blobId);
    if (!blob) {
        ALOGE("Fail to get blob for lp mode(%" PRId64 ")", blobId);
        return -EINVAL;
    }
    int lp_mode_num = blob->length / sizeof(struct _drmModeModeInfo);
    ALOGD("lp mode count : %d, length %d", lp_mode_num, blob->length);
    char *dozeModePtr = (char *)(blob->data);

    for (int i = 0; i < lp_mode_num; i++) {
        drmModeModeInfo *dozeModeInfo =
            (drmModeModeInfo *)(dozeModePtr + (sizeof(struct _drmModeModeInfo)) * i);
        uint32_t dozeModeId = mDrmDevice->next_mode_id();
        mDozeDrmModes.insert(std::make_pair(dozeModeId, DrmMode(dozeModeInfo)));
        mDozeDrmModes[dozeModeId].set_id(dozeModeId);
        ALOGD("Doze mode id(%d): width :%d, height : %d, vsync : %f",
              mDozeDrmModes[dozeModeId].id(), mDozeDrmModes[dozeModeId].h_display(),
              mDozeDrmModes[dozeModeId].v_display(), mDozeDrmModes[dozeModeId].v_refresh());
    }

    drmModeFreePropertyBlob(blob);

    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::setLowPowerMode(bool suspend) {
    if (!isDozeModeAvailable()) {
        return HWC2_ERROR_UNSUPPORTED;
    }

    int ret = 0;
    int modeNo = -1;

    for (auto &e : mDozeDrmModes) {
        ALOGD("%s, %d, %d, %d, %d", __func__, e.second.h_display(), e.second.v_display(),
              mActiveModeState.mode.h_display(), mActiveModeState.mode.v_display());
        if ((e.second.h_display() == mActiveModeState.mode.h_display()) &&
            (e.second.v_display() == mActiveModeState.mode.v_display())) {
            modeNo = e.first;
        }
    }

    if (modeNo < 0) {
        ALOGE("%s : Can't find Dozemode", __func__);
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (!suspend)
        setPowerMode(HWC_POWER_MODE_NORMAL);

    ret = setActiveDrmMode(mDozeDrmModes[modeNo]);

    if (suspend)
        setPowerMode(HWC_POWER_MODE_OFF);

    return ret;
}

int32_t ExynosDisplayDrmInterface::setPowerMode(int32_t mode) {
    int ret = 0;
    uint64_t dpms_value = 0;
    if (mode == HWC_POWER_MODE_OFF) {
        dpms_value = DRM_MODE_DPMS_OFF;
    } else {
        if (mDisplayIdentifier.type == HWC_DISPLAY_VIRTUAL) {
            chosePreferredConfig();
        }
        dpms_value = DRM_MODE_DPMS_ON;
    }

    if (mDrmConnector == nullptr)
        return ret;
    const DrmProperty &prop = mDrmConnector->dpms_property();
    if ((ret = drmModeConnectorSetProperty(mDrmDevice->fd(), mDrmConnector->id(), prop.id(),
                                           dpms_value)) != NO_ERROR) {
        HWC_LOGE(mDisplayIdentifier, "setPower mode ret (%d)", ret);
    }

    if (mode == HWC_POWER_MODE_OFF) {
        if (mDisplayIdentifier.type == HWC_DISPLAY_VIRTUAL) {
            if ((ret = clearActiveDrmMode()) != NO_ERROR) {
                HWC_LOGE(mDisplayIdentifier, "%s: Fail to clear display mode", __func__);
            }
        }
    }
    return ret;
}

int32_t ExynosDisplayDrmInterface::setVsyncEnabled(uint32_t enabled) {
    mDrmVSyncWorker.VSyncControl(HWC2_VSYNC_ENABLE == enabled);
    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::chosePreferredConfig() {
    uint32_t num_configs = 0;
    std::map<uint32_t, displayConfigs_t> temp;
    int32_t err = getDisplayConfigs(&num_configs, NULL, temp);
    if (err != HWC2_ERROR_NONE || !num_configs)
        return err;

    mPreferredModeId = mDrmConnector->get_preferred_mode_id();
    ALOGI("Preferred mode id: %d", mPreferredModeId);
    err = setActiveConfig(mPreferredModeId, temp[mPreferredModeId]);
    return err;
}

int32_t ExynosDisplayDrmInterface::getDPUConfig(hwc2_config_t *config) {
    *config = mDrmConnector->get_preferred_mode_id();
    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::getDisplayConfigs(uint32_t *outNumConfigs,
                                                     hwc2_config_t *outConfigs, std::map<uint32_t, displayConfigs_t> &displayConfigs) {
    if (!outConfigs) {
        int ret = mDrmConnector->UpdateModes();
        if (ret) {
            ALOGE("Failed to update display modes %d", ret);
            return HWC2_ERROR_BAD_DISPLAY;
        }
        dumpDisplayConfigs();

        displayConfigs.clear();

        uint32_t mm_width = mDrmConnector->mm_width();
        uint32_t mm_height = mDrmConnector->mm_height();

        /* key: (width<<32 | height) */
        std::map<uint64_t, uint32_t> groupIds;
        uint32_t groupId = 0;

        for (const DrmMode &mode : mDrmConnector->modes()) {
            displayConfigs_t configs;
            configs.vsyncPeriod = nsecsPerSec / mode.v_refresh();
            configs.width = mode.h_display();
            configs.height = mode.v_display();
            uint64_t key = ((uint64_t)configs.width << 32) | configs.height;
            auto it = groupIds.find(key);
            if (it != groupIds.end()) {
                configs.groupId = it->second;
            } else {
                groupIds.insert(std::make_pair(key, groupId));
                configs.groupId = groupId;
                groupId++;
            }

            // Dots per 1000 inches
            configs.Xdpi = mm_width ? (mode.h_display() * kUmPerInch) / mm_width : -1;
            // Dots per 1000 inches
            configs.Ydpi = mm_height ? (mode.v_display() * kUmPerInch) / mm_height : -1;
            displayConfigs.insert(std::make_pair(mode.id(), configs));
            ALOGD("config group(%d), w(%d), h(%d), vsync(%d), xdpi(%d), ydpi(%d)",
                  configs.groupId, configs.width, configs.height,
                  configs.vsyncPeriod, configs.Xdpi, configs.Ydpi);
        }
    }

    uint32_t num_modes = static_cast<uint32_t>(mDrmConnector->modes().size());
    if (!outConfigs) {
        *outNumConfigs = num_modes;
        return HWC2_ERROR_NONE;
    }

    uint32_t idx = 0;

    for (const DrmMode &mode : mDrmConnector->modes()) {
        if (idx >= *outNumConfigs)
            break;
        outConfigs[idx++] = mode.id();
    }
    *outNumConfigs = idx;

    return 0;
}

void ExynosDisplayDrmInterface::dumpDisplayConfigs() {
    uint32_t num_modes = static_cast<uint32_t>(mDrmConnector->modes().size());
    for (uint32_t i = 0; i < num_modes; i++) {
        auto mode = mDrmConnector->modes().at(i);
        ALOGD("%s display config[%d] %s:: id(%d), clock(%d), flags(%d), type(%d)",
              mDisplayIdentifier.name.c_str(), i, mode.name().c_str(), mode.id(), mode.clock(), mode.flags(), mode.type());
        ALOGD("\th_display(%d), h_sync_start(%d), h_sync_end(%d), h_total(%d), h_skew(%d)",
              mode.h_display(), mode.h_sync_start(), mode.h_sync_end(), mode.h_total(), mode.h_skew());
        ALOGD("\tv_display(%d), v_sync_start(%d), v_sync_end(%d), v_total(%d), v_scan(%d), v_refresh(%f)",
              mode.v_display(), mode.v_sync_start(), mode.v_sync_end(), mode.v_total(), mode.v_scan(), mode.v_refresh());
    }
}

int32_t ExynosDisplayDrmInterface::getDisplayVsyncPeriod(hwc2_vsync_period_t *outVsyncPeriod) {
    int ret = 0;
    if (mDrmConnector->adjusted_fps().id() == 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to get adjusted_fps id");
        return HWC2_ERROR_UNSUPPORTED;
    }

    if ((ret = mDrmDevice->UpdateConnectorProperty(*mDrmConnector,
                                                   &mDrmConnector->adjusted_fps())) != 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to update fps property update");
        return ret;
    }

    uint64_t fps_;
    std::tie(ret, fps_) = mDrmConnector->adjusted_fps().value();

    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to get adjusted_fps_property value");
        return ret;
    }

    *outVsyncPeriod = (1000000000 / (float)fps_);

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::getConfigChangeDuration() {
    /* TODO: Get from driver */
    return 2;
};

int32_t ExynosDisplayDrmInterface::getVsyncAppliedTime(hwc2_config_t configId,
                                                       displayConfigs &config, int64_t *actualChangeTime) {
    if (mDrmCrtc->adjusted_vblank_property().id() == 0) {
        uint64_t currentTime = systemTime(SYSTEM_TIME_MONOTONIC);
        *actualChangeTime = currentTime +
                            (config.vsyncPeriod) * getConfigChangeDuration();
        return HWC2_ERROR_NONE;
    }

    int ret = 0;
    if ((ret = mDrmDevice->UpdateCrtcProperty(*mDrmCrtc,
                                              &mDrmCrtc->adjusted_vblank_property())) != 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to update vblank property");
        return ret;
    }

    uint64_t timestamp;
    std::tie(ret, timestamp) = mDrmCrtc->adjusted_vblank_property().value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to get vblank property");
        return ret;
    }

    *actualChangeTime = static_cast<int64_t>(timestamp);
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::getColorModes(
    uint32_t *outNumModes,
    int32_t *outModes) {
    if (mDrmCrtc->color_mode_property().id() == 0) {
        *outNumModes = 1;

        if (outModes != NULL) {
            outModes[0] = HAL_COLOR_MODE_NATIVE;
        }
        return HWC2_ERROR_NONE;
    }

    uint32_t colorNum = 0;
    for (auto &e : mColorModeEnums) {
        if (outModes != NULL) {
            outModes[colorNum] = e.first;
        }
        colorNum++;
        ALOGD("Colormode [hal: %d, drm: %" PRId64 "]", e.first, e.second);
    }
    *outNumModes = colorNum;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::setColorTransform(const float *matrix,
                                                     int32_t hint, int32_t dqe_fd) {
#ifdef USE_DISPLAY_COLOR_INTERFACE
    if (mDrmCrtc->color_mode_property().id() == 0) {
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (mDrmCrtc->dqe_fd_property().id() == 0) {
        ALOGI("%s:: dqe_fd property id is abnormal", __func__);
        return HWC2_ERROR_UNSUPPORTED;
    } else {
        mColorRequest.requestTransform(dqe_fd);
    }
#endif
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::setColorMode(int32_t mode, int32_t dqe_fd) {
    if (mDrmCrtc->color_mode_property().id() == 0) {
        return HWC2_ERROR_NONE;
    }
#ifndef USE_DISPLAY_COLOR_INTERFACE
    dqe_fd = -1;
#endif
    mColorRequest.requestColorMode(dqe_fd, mode);
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::setColorModeWithRenderIntent(int32_t mode, int32_t intent,
                                                                int32_t dqe_fd) {
    if (mDrmCrtc->color_mode_property().id() == 0) {
        return HWC2_ERROR_NONE;
    }
    if (mDrmCrtc->render_intent_property().id() == 0) {
        ALOGI("%s:: render_intent propety id is abnormal", __func__);
        return HWC2_ERROR_NONE;
    }
#ifndef USE_DISPLAY_COLOR_INTERFACE
    dqe_fd = -1;
#endif
    mColorRequest.requestColorMode(dqe_fd, mode, intent);
    return HWC2_ERROR_NONE;
}

void ExynosDisplayDrmInterface::getDisplayHWInfo(uint32_t __unused &xres,
                                                 uint32_t __unused &yres, int &psrMode, std::vector<ResolutionInfo> &resolutionInfo) {
    switch (std::get<1>((mDrmCrtc->operation_mode_property().value()))) {
    case DECON_MIPI_COMMAND_MODE:
        psrMode = PSR_MIPI;
        break;
    case DECON_VIDEO_MODE:
    default:
        psrMode = PSR_NONE;
        break;
    }
}

int32_t ExynosDisplayDrmInterface::setActiveConfigWithConstraints(
    hwc2_config_t config, displayConfigs_t &displayConfig, bool test) {
    HDEBUGLOGD(eDebugDisplayConfig, "%s:: %s config(%d)", __func__,
               mDisplayIdentifier.name.c_str(), config);

    auto mode = std::find_if(mDrmConnector->modes().begin(), mDrmConnector->modes().end(),
                             [config](DrmMode const &m) { return m.id() == config; });
    if (mode == mDrmConnector->modes().end()) {
        HWC_LOGE(mDisplayIdentifier, "Could not find active mode for %d", config);
        return HWC2_ERROR_BAD_CONFIG;
    }

    if ((mActiveModeState.blob_id != 0) &&
        (mActiveModeState.mode.id() == config)) {
        ALOGD("%s:: same mode %d", __func__, config);
        return HWC2_ERROR_NONE;
    }

    if (mDesiredModeState.needs_modeset) {
        ALOGD("Previous mode change request is not applied");
    }

    int32_t ret = HWC2_ERROR_NONE;
    DrmModeAtomicReq drmReq(this);
    uint32_t modeBlob = 0;
    if (mDesiredModeState.mode.id() != config) {
        if ((ret = createModeBlob(*mode, modeBlob)) != NO_ERROR) {
            HWC_LOGE(mDisplayIdentifier, "%s: Fail to set mode state",
                     __func__);
            return HWC2_ERROR_BAD_CONFIG;
        }
    }
    if (test) {
        if ((ret = setDisplayMode(drmReq, modeBlob ? modeBlob : mDesiredModeState.blob_id)) < 0) {
            HWC_LOGE(mDisplayIdentifier, "%s: Fail to apply display mode",
                     __func__);
            return ret;
        }
        ret = drmReq.commit(DRM_MODE_ATOMIC_TEST_ONLY, true);
        if (ret) {
            drmReq.addOldBlob(modeBlob);
            HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in applyDisplayMode()\n",
                     __func__, ret);
            return ret;
        }
    } else {
        mDesiredModeState.needs_modeset = true;
    }

    if (modeBlob != 0) {
        mDesiredModeState.setMode(*mode, modeBlob, drmReq);
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::setActiveDrmMode(DrmMode const &mode) {
    if ((mActiveModeState.blob_id != 0) &&
        (mActiveModeState.mode.id() == mode.id())) {
        ALOGD("%s:: same mode %d", __func__, mode.id());
        return HWC2_ERROR_NONE;
    }

    int32_t ret = HWC2_ERROR_NONE;
    uint32_t modeBlob;
    if ((ret = createModeBlob(mode, modeBlob)) != NO_ERROR) {
        HWC_LOGE(mDisplayIdentifier, "%s: Fail to set mode state",
                 __func__);
        return HWC2_ERROR_BAD_CONFIG;
    }

    DrmModeAtomicReq drmReq(this);

    if ((ret = setDisplayMode(drmReq, modeBlob)) != NO_ERROR) {
        drmReq.addOldBlob(modeBlob);
        HWC_LOGE(mDisplayIdentifier, "%s: Fail to apply display mode",
                 __func__);
        return ret;
    }
    setWorkingVsyncPeriodProp(drmReq);

    ALOGD("%s, commit  %d -> %d", __func__, mActiveModeState.mode.id(), mode.id());
    if ((ret = drmReq.commit(DRM_MODE_ATOMIC_ALLOW_MODESET, true))) {
        drmReq.addOldBlob(modeBlob);
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in applyDisplayMode()\n",
                 __func__, ret);
        return ret;
    }

    if (mDisplayIdentifier.type == HWC_DISPLAY_PRIMARY) {
        String8 strDispW;
        String8 strDispH;
        strDispW.appendFormat("%d", mode.h_display());
        strDispH.appendFormat("%d", mode.v_display());
        property_set("vendor.hwc.display.w", strDispW.c_str());
        property_set("vendor.hwc.display.h", strDispH.c_str());
    }

    mDrmConnector->set_active_mode(mode);
    mActiveModeState.setMode(mode, modeBlob, drmReq);
    mActiveModeState.needs_modeset = false;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayDrmInterface::createModeBlob(const DrmMode &mode,
                                                  uint32_t &modeBlob) {
    struct drm_mode_modeinfo drm_mode;
    memset(&drm_mode, 0, sizeof(drm_mode));
    mode.ToDrmModeModeInfo(&drm_mode);

    modeBlob = 0;
    int ret = mDrmDevice->CreatePropertyBlob(&drm_mode, sizeof(drm_mode),
                                             &modeBlob);
    if (ret) {
        HWC_LOGE(mDisplayIdentifier, "Failed to create mode property blob %d", ret);
        return ret;
    }

    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::setDisplayMode(
    DrmModeAtomicReq &drmReq, const uint32_t modeBlob) {
    int ret = NO_ERROR;

    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->mode_property(), modeBlob)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->modeset_only_property(), 1)) < 0) {
        ALOGW("modeset_only is not supported");
        ret = BAD_TYPE;
    }

    if ((ret = drmReq.atomicAddProperty(mDrmConnector->id(),
                                        mDrmConnector->crtc_id_property(), mDrmCrtc->id())) < 0)
        return ret;

    return ret;
}

int32_t ExynosDisplayDrmInterface::clearActiveDrmMode() {
    ALOGD("%s:: %s", __func__, mDisplayIdentifier.name.c_str());
    int ret = NO_ERROR;
    DrmModeAtomicReq drmReq(this);

    if (mActiveModeState.blob_id) {
        mDrmDevice->DestroyPropertyBlob(mActiveModeState.blob_id);
        mActiveModeState.reset();
    }

    if ((ret = drmReq.atomicAddProperty(mDrmConnector->id(),
                                        mDrmConnector->crtc_id_property(), 0)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->modeset_only_property(), 0)) < 0) {
        ALOGW("modeset_only is not supported");
    }

    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->mode_property(), 0)) < 0)
        return ret;

    if ((ret = drmReq.commit(DRM_MODE_ATOMIC_ALLOW_MODESET, true))) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in applyDisplayMode()\n",
                 __func__, ret);
        return ret;
    }

    return ret;
}

int32_t ExynosDisplayDrmInterface::setActiveConfig(hwc2_config_t config,
                                                   displayConfigs_t __unused &displayConfig) {
    ALOGD("%s:: %s config(%d)", __func__, mDisplayIdentifier.name.c_str(), config);
    auto mode = std::find_if(mDrmConnector->modes().begin(), mDrmConnector->modes().end(),
                             [config](DrmMode const &m) { return m.id() == config; });
    if (mode == mDrmConnector->modes().end()) {
        HWC_LOGE(mDisplayIdentifier, "Could not find active mode for %d", config);
        return HWC2_ERROR_BAD_CONFIG;
    }

    if (!setActiveDrmMode(*mode))
        ALOGI("%s:: %s config(%d)", __func__, mDisplayIdentifier.name.c_str(), config);

    /* Init previous desired mode set request */
    resetConfigRequestState();
    return 0;
}

int32_t ExynosDisplayDrmInterface::setCursorPositionAsync(uint32_t x_pos, uint32_t y_pos) {
    return 0;
}

int32_t ExynosDisplayDrmInterface::updateHdrCapabilities(std::vector<int32_t> &outTypes,
                                                         float *outMaxLuminance, float *outMaxAverageLuminance, float *outMinLuminance) {
    const DrmProperty &prop_max_luminance = mDrmConnector->max_luminance();
    const DrmProperty &prop_max_avg_luminance = mDrmConnector->max_avg_luminance();
    const DrmProperty &prop_min_luminance = mDrmConnector->min_luminance();
    const DrmProperty &prop_hdr_formats = mDrmConnector->hdr_formats();

    int ret = 0;
    uint64_t max_luminance = 0;
    uint64_t max_avg_luminance = 0;
    uint64_t min_luminance = 0;
    uint64_t hdr_formats = 0;

    if ((prop_max_luminance.id() == 0) ||
        (prop_max_avg_luminance.id() == 0) ||
        (prop_min_luminance.id() == 0) ||
        (prop_hdr_formats.id() == 0)) {
        ALOGE("%s:: there is no property for hdrCapabilities (max_luminance: %d, max_avg_luminance: %d, min_luminance: %d, hdr_formats: %d",
              __func__, prop_max_luminance.id(), prop_max_avg_luminance.id(),
              prop_min_luminance.id(), prop_hdr_formats.id());
        return -1;
    }

    std::tie(ret, max_luminance) = prop_max_luminance.value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: there is no max_luminance (ret = %d)",
                 __func__, ret);
        return -1;
    }

    std::tie(ret, max_avg_luminance) = prop_max_avg_luminance.value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: there is no max_avg_luminance (ret = %d)",
                 __func__, ret);
        return -1;
    }

    std::tie(ret, min_luminance) = prop_min_luminance.value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: there is no min_luminance (ret = %d)",
                 __func__, ret);
        return -1;
    }

    if (mIsHdrSink) {
        *outMaxLuminance = 50 * pow(2.0, (double)max_luminance / 32);
        *outMaxAverageLuminance = 50 * pow(2.0, (double)max_avg_luminance / 32);
        *outMinLuminance = (*outMaxLuminance) * static_cast<float>(min_luminance * min_luminance) / CONSTANT_FOR_DP_MIN_LUMINANCE;
    } else {
        *outMaxLuminance = (float)max_luminance / DISPLAY_LUMINANCE_UNIT;
        *outMaxAverageLuminance = (float)max_avg_luminance / DISPLAY_LUMINANCE_UNIT;
        *outMinLuminance = (float)min_luminance / DISPLAY_LUMINANCE_UNIT;
    }

    std::tie(ret, hdr_formats) = prop_hdr_formats.value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: there is no hdr_formats (ret = %d)",
                 __func__, ret);
        return -1;
    }

    uint32_t typeBit;
    outTypes.clear();
    std::tie(typeBit, ret) = prop_hdr_formats.GetEnumValueWithName("Dolby Vision");
    if ((ret == 0) && (hdr_formats & (1 << typeBit))) {
        outTypes.push_back(HAL_HDR_DOLBY_VISION);
        ALOGI("%s: supported hdr types : %d",
              mDisplayIdentifier.name.c_str(), HAL_HDR_DOLBY_VISION);
    }
    std::tie(typeBit, ret) = prop_hdr_formats.GetEnumValueWithName("HDR10");
    if ((ret == 0) && (hdr_formats & (1 << typeBit))) {
        outTypes.push_back(HAL_HDR_HDR10);
        ALOGI("%s: supported hdr types : %d",
              mDisplayIdentifier.name.c_str(), HAL_HDR_HDR10);
    }
    std::tie(typeBit, ret) = prop_hdr_formats.GetEnumValueWithName("HLG");
    if ((ret == 0) && (hdr_formats & (1 << typeBit))) {
        outTypes.push_back(HAL_HDR_HLG);
        ALOGI("%s: supported hdr types : %d",
              mDisplayIdentifier.name.c_str(), HAL_HDR_HLG);
    }
    std::tie(typeBit, ret) = prop_hdr_formats.GetEnumValueWithName("HDR10_PLUS");
    if ((ret == 0) && (hdr_formats & (1 << typeBit))) {
        outTypes.push_back(HAL_HDR_HDR10_PLUS);
        ALOGI("%s: supported hdr types : %d",
              mDisplayIdentifier.name.c_str(), HAL_HDR_HDR10_PLUS);
    }

    ALOGI("get hdrCapabilities info max_luminance(%" PRId64 "), "
          "max_avg_luminance(%" PRId64 "), min_luminance(%" PRId64 "), "
          "hdr_formats(0x%" PRIx64 ")",
          max_luminance, max_avg_luminance, min_luminance, hdr_formats);

    ALOGI("hdrTypeNum(%u), maxLuminance(%f), maxAverageLuminance(%f), minLuminance(%f)",
          static_cast<uint32_t>(outTypes.size()), *outMaxLuminance, *outMaxAverageLuminance, *outMinLuminance);

    return 0;
}

int32_t ExynosDisplayDrmInterface::getBufferId(
    const exynos_win_config_data &config, uint32_t &fbId, const bool useCache) {
    int ret = NO_ERROR;
    if ((ret = mFBManager.getBuffer(mDisplayIdentifier.type, config, fbId, useCache)) < 0) {
        HWC_LOGE(mDisplayIdentifier,
                 "%s:: Failed to get FB, fbId(%d), ret(%d)",
                 __func__, fbId, ret);
        return ret;
    }

    if (!useCache)
        mFbIds.push_back(fbId);

    return ret;
}

int32_t ExynosDisplayDrmInterface::setupCommitFromDisplayConfig(
    ExynosDisplayDrmInterface::DrmModeAtomicReq &drmReq,
    const exynos_win_config_data &config,
    const uint32_t configIndex,
    const std::unique_ptr<DrmPlane> &plane,
    uint32_t &fbId) {
    int ret = NO_ERROR;

    bool useCache = (config.state == config.WIN_STATE_COLOR) ? false : kUseBufferCaching;
    if ((fbId == 0) &&
        ((ret = getBufferId(config, fbId, useCache)) < 0)) {
        return ret;
    }

    if (((ret = drmReq.atomicAddProperty(plane->id(), plane->crtc_property(),
                                         mDrmCrtc->id())) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(), plane->fb_property(),
                                         fbId)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->crtc_x_property(), config.dst.x)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->crtc_y_property(), config.dst.y)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->crtc_w_property(), config.dst.w)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->crtc_h_property(), config.dst.h)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(), plane->src_x_property(),
                                         (int)(config.src.x) << 16)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(), plane->src_y_property(),
                                         (int)(config.src.y) << 16)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(), plane->src_w_property(),
                                         (int)(config.src.w) << 16)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(), plane->src_h_property(),
                                         (int)(config.src.h) << 16)) < 0) ||
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->rotation_property(),
                                         halTransformToDrmRot(config.transform), true)) < 0)) {
        HWC_LOGE(mDisplayIdentifier, "Fail to set properties");
        return ret;
    }

    auto setEnumProperty = [&](const int32_t halData,
                               const DrmPropertyMap &drmEnums,
                               const DrmProperty &property) -> int32_t {
        int32_t retVal = NO_ERROR;
        uint64_t drmEnum = 0;
        std::tie(drmEnum, retVal) = halToDrmEnum(halData, drmEnums);
        if ((retVal < 0) || ((retVal = drmReq.atomicAddProperty(plane->id(),
                                                                property, drmEnum, true)) < 0)) {
            HWC_LOGE(mDisplayIdentifier, "Fail to set %s (%d)",
                     property.name().c_str(), halData);
            return retVal;
        }
        return NO_ERROR;
    };

    if ((ret = setEnumProperty(config.blending, mBlendEnums,
                               plane->blend_property()) < 0))
        return ret;

    if (plane->zpos_property().id() &&
        !plane->zpos_property().is_immutable()) {
        uint64_t min_zpos = 0;

        // Ignore ret and use min_zpos as 0 by default
        std::tie(std::ignore, min_zpos) = plane->zpos_property().range_min();

        if ((ret = drmReq.atomicAddProperty(plane->id(),
                                            plane->zpos_property(), configIndex + min_zpos)) < 0)
            return ret;
    }

    if (plane->alpha_property().id()) {
        uint64_t min_alpha = 0;
        uint64_t max_alpha = 0;
        std::tie(std::ignore, min_alpha) = plane->alpha_property().range_min();
        std::tie(std::ignore, max_alpha) = plane->alpha_property().range_max();

        uint64_t planeAlpha = (uint64_t)(((max_alpha - min_alpha) * config.plane_alpha) + 0.5) + min_alpha;
        if (!((planeAlpha >= min_alpha) && (planeAlpha <= max_alpha))) {
            planeAlpha = planeAlpha > max_alpha ? max_alpha : min_alpha;
            ALOGW("[%s] Invalid plane alpha (%f)", mDisplayIdentifier.name.c_str(), config.plane_alpha);
        }

        if ((ret = drmReq.atomicAddProperty(plane->id(),
                                            plane->alpha_property(),
                                            planeAlpha, true)) < 0)
            return ret;
    }

    if ((config.acq_fence >= 0) &&
        ((ret = drmReq.atomicAddProperty(plane->id(),
                                         plane->in_fence_fd_property(), config.acq_fence)) < 0))
        return ret;

    if (config.state == config.WIN_STATE_COLOR) {
        if (plane->colormap_property().id()) {
            if ((ret = drmReq.atomicAddProperty(plane->id(),
                                                plane->colormap_property(), config.color)) < 0)
                return ret;
        } else {
            HWC_LOGE(mDisplayIdentifier, "colormap property is not supported");
        }
    }

    if ((ret = setEnumProperty(config.dataspace & HAL_DATASPACE_STANDARD_MASK,
                               mStandardEnums, plane->standard_property()) < 0) ||
        (ret = setEnumProperty(config.dataspace & HAL_DATASPACE_TRANSFER_MASK,
                               mTransferEnums, plane->transfer_property()) < 0) ||
        (ret = setEnumProperty(config.dataspace & HAL_DATASPACE_RANGE_MASK,
                               mRangeEnums, plane->range_property()) < 0))
        return ret;

    if (hasHdrInfo(config.dataspace)) {
        /* Static metadata for external display */
        setFrameStaticMeta(drmReq, config);
    }

    if (plane->hdr_fd_property().id()) {
        if ((ret = drmReq.atomicAddProperty(plane->id(),
                                            plane->hdr_fd_property(), config.fd_lut)) < 0)
            return ret;
    }

    if (plane->virtual8k_split_property().id()) {
        if ((ret = drmReq.atomicAddProperty(plane->id(),
                                            plane->virtual8k_split_property(), config.split, true)) < 0)
            return ret;
    }

    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::setFrameStaticMeta(DrmModeAtomicReq &drmReq,
                                                      const exynos_win_config_data &config) {
    if (!mDrmConnector->hdr_output_meta().id())
        return NO_ERROR;

    int ret = NO_ERROR;

    if (config.metaParcel != nullptr) {
        struct hdr_output_metadata drm_hdr_meta;
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[0].x =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mR.x);
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[0].y =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mR.y);
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[1].x =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mG.x);
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[1].y =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mG.y);
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[2].x =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mB.x);
        drm_hdr_meta.hdmi_metadata_type1.display_primaries[2].y =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mB.y);
        drm_hdr_meta.hdmi_metadata_type1.max_display_mastering_luminance =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance / 10000);
        drm_hdr_meta.hdmi_metadata_type1.min_display_mastering_luminance =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance / 10000);
        drm_hdr_meta.hdmi_metadata_type1.max_cll =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mMaxContentLightLevel);
        drm_hdr_meta.hdmi_metadata_type1.max_fall =
            static_cast<__u16>(config.metaParcel->sHdrStaticInfo.sType1.mMaxFrameAverageLightLevel);

        uint32_t blob_id = 0;
        ret = mDrmDevice->CreatePropertyBlob(&drm_hdr_meta, sizeof(drm_hdr_meta), &blob_id);
        if (ret || (blob_id == 0)) {
            HWC_LOGE(mDisplayIdentifier, "Failed to create static meta"
                                         "blob id=%d, ret=%d",
                     blob_id, ret);
            return ret;
        }

        if ((ret = drmReq.atomicAddProperty(mDrmConnector->id(),
                                            mDrmConnector->hdr_output_meta(),
                                            blob_id)) < 0) {
            HWC_LOGE(mDisplayIdentifier, "Failed to set hdr_output_meta property %d", ret);
            return ret;
        }
    }

    return ret;
}

int32_t ExynosDisplayDrmInterface::setupPartialRegion(
    exynos_dpu_data &dpuData, DrmModeAtomicReq &drmReq) {
    if (!mDrmCrtc->partial_region_property().id())
        return NO_ERROR;

    int ret = NO_ERROR;

    struct decon_frame &update_region = dpuData.win_update_region;
    struct drm_clip_rect partial_rect = {
        static_cast<unsigned short>(update_region.x),
        static_cast<unsigned short>(update_region.y),
        static_cast<unsigned short>(update_region.x + update_region.w),
        static_cast<unsigned short>(update_region.y + update_region.h),
    };
    if ((mPartialRegionState.blob_id == 0) ||
        mPartialRegionState.isUpdated(partial_rect)) {
        uint32_t blob_id = 0;
        ret = mDrmDevice->CreatePropertyBlob(&partial_rect,
                                             sizeof(partial_rect), &blob_id);
        if (ret || (blob_id == 0)) {
            HWC_LOGE(mDisplayIdentifier, "Failed to create partial region "
                                         "blob id=%d, ret=%d",
                     blob_id, ret);
            return ret;
        }

        HDEBUGLOGD(eDebugWindowUpdate,
                   "%s: partial region updated [%d, %d, %d, %d] -> [%d, %d, %d, %d] blob(%d)",
                   mDisplayIdentifier.name.c_str(),
                   mPartialRegionState.partial_rect.x1,
                   mPartialRegionState.partial_rect.y1,
                   mPartialRegionState.partial_rect.x2,
                   mPartialRegionState.partial_rect.y2,
                   partial_rect.x1,
                   partial_rect.y1,
                   partial_rect.x2,
                   partial_rect.y2,
                   blob_id);
        mPartialRegionState.partial_rect = partial_rect;

        if (mPartialRegionState.blob_id)
            drmReq.addOldBlob(mPartialRegionState.blob_id);
        mPartialRegionState.blob_id = blob_id;
    }
    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->partial_region_property(),
                                        mPartialRegionState.blob_id)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "Failed to set partial region property %d", ret);
        return ret;
    }

    return ret;
}

void ExynosDisplayDrmInterface::disablePlanes(DrmModeAtomicReq &drmReq,
                                              uint32_t *planeEnableInfo) {
    /*
     * This function disables the plane that is reserved to this display.
     * If planeEnableInfo is nullptr, disable all of planes that is reserved to
     * this display.
     * If planeEnableInfo is not nullptr, disable planes that
     * planeEnableInfo[plane->id()] value is 0.
     */
    uint32_t chId = 0;
    for (auto &plane : mDrmDevice->planes()) {
        uint32_t curChId = chId++;
        ExynosMPP *exynosMPP = mExynosMPPsForPlane[plane->id()];
        if (((exynosMPP != nullptr) && (mDisplayIdentifier.id != UINT32_MAX) &&
             (exynosMPP->mAssignedState & MPP_ASSIGN_STATE_RESERVED) &&
             (exynosMPP->mReservedDisplayInfo.displayIdentifier.id !=
              (int32_t)mDisplayIdentifier.id) &&
             (!mCanDisableAllPlanes)) ||
            ((planeEnableInfo != nullptr) &&
             (planeEnableInfo[curChId] == 1)))
            continue;

        if (drmReq.atomicAddProperty(plane->id(), plane->crtc_property(), 0) < 0) {
            HWC_LOGE(mDisplayIdentifier, "%s:: Fail to add crtc_property",
                     __func__);
            continue;
        }
        if (drmReq.atomicAddProperty(plane->id(), plane->fb_property(), 0) < 0) {
            HWC_LOGE(mDisplayIdentifier, "%s:: Fail to add fb_property",
                     __func__);
            continue;
        }
    }
}

void ExynosDisplayDrmInterface::flipFBs(bool isActiveCommit) {
    if (mFbIds.size() > 0) {
        if (isActiveCommit) {
            removeFbs(mDrmDevice->fd(), mOldFbIds);
            mOldFbIds = mFbIds;
        } else {
            removeFbs(mDrmDevice->fd(), mFbIds);
        }
        mFbIds.clear();
    }
    mFBManager.flip(mDisplayIdentifier.type, isActiveCommit);
}

int32_t ExynosDisplayDrmInterface::deliverWinConfigData(exynos_dpu_data &dpuData) {
    int ret = NO_ERROR;
    uint32_t planeEnableInfo[MAX_DECON_WIN] = {0};
    android::String8 result;

    funcReturnCallback retCallback(
        [&]() {
            flipFBs((ret == NO_ERROR) && !mDrmReq.getError());
            mDrmReq.reset(); });

    if (mDesiredModeState.needs_modeset) {
        /* Use different instance with mDrmReq */
        DrmModeAtomicReq drmReqForModeSet(this);

        int _ret = setDisplayMode(drmReqForModeSet, mDesiredModeState.blob_id);

        /* If modeset_only property is supported, disable all planes is not needed
         * If it's not supported, black screen will be displayed */
        if (_ret == BAD_TYPE)
            disablePlanes(drmReqForModeSet);
        else if (_ret < 0)
            return _ret;

        setWorkingVsyncPeriodProp(drmReqForModeSet);

        if ((ret = drmReqForModeSet.commit(DRM_MODE_ATOMIC_ALLOW_MODESET, true)) < 0) {
            HWC_LOGE(mDisplayIdentifier, "%s: Fail to apply display mode",
                     __func__);
            return ret;
        }
    }

    if (dpuData.enable_readback &&
        (ret = setupConcurrentWritebackCommit(dpuData, mDrmReq)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to setup concurrent writeback commit ret(%d)",
                 __func__, ret);
        return ret;
    }

    if (dpuData.enable_standalone_writeback &&
        (ret = setupStandaloneWritebackCommit(dpuData, mDrmReq)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to setup standalone writeback commit ret(%d)",
                 __func__, ret);
        return ret;
    }

    if ((ret = setupPartialRegion(dpuData, mDrmReq)) != NO_ERROR)
        return ret;

    uint64_t out_fences[mDrmDevice->crtcs().size()];
    if ((ret = mDrmReq.atomicAddProperty(mDrmCrtc->id(),
                                         mDrmCrtc->out_fence_ptr_property(),
                                         (uint64_t)&out_fences[mDrmCrtc->pipe()], true)) < 0) {
        return ret;
    }

    size_t virtualPlaneIndex = 0;
    for (exynos_win_config_data &config : dpuData.configs) {
        if ((config.state != config.WIN_STATE_BUFFER) &&
            (config.state != config.WIN_STATE_COLOR) &&
            (config.state != config.WIN_STATE_CURSOR))
            continue;

        virtual8KOTFInfo virtualOTFInfo;
        bool needVirtual8K = getVirtual8KOtfInfo(config, virtualOTFInfo);

        int channelId = config.assignedMPP->mChId;

        /* src size should be set even in dim layer */
        if (config.state == config.WIN_STATE_COLOR) {
            config.src.w = config.dst.w;
            config.src.h = config.dst.h;
        }

        auto setupCommit = [&](int32_t chId, exynos_win_config_data &conf) -> int32_t {
            if ((chId < 0) || (chId >= mDrmDevice->planes().size()))
                ret = -EINVAL;
            auto &plane = mDrmDevice->planes().at(chId);
            uint32_t fbId = 0;
            if ((ret = setupCommitFromDisplayConfig(mDrmReq, conf, virtualPlaneIndex, plane, fbId)) < 0) {
                HWC_LOGE(mDisplayIdentifier, "setupCommitFromDisplayConfig failed, config[%zu]", virtualPlaneIndex);
                return ret;
            }
            planeEnableInfo[chId] = 1;
            virtualPlaneIndex++;
            return NO_ERROR;
        };

        int32_t chId = needVirtual8K ? virtualOTFInfo.firstHalf.channelId : channelId;
        exynos_win_config_data &conf = needVirtual8K ? virtualOTFInfo.firstHalf.config : config;

        if ((ret = setupCommit(chId, conf)) < 0) {
            HWC_LOGE(mDisplayIdentifier,
                     "setupCommitFromDisplayConfig failed, config[%zu], "
                     "needVirtual8K(%d), chId(%d)",
                     virtualPlaneIndex,
                     needVirtual8K, chId);
            return ret;
        }

        if (needVirtual8K &&
            ((ret = setupCommit(virtualOTFInfo.lastHalf.channelId,
                                virtualOTFInfo.lastHalf.config)) < 0)) {
            HWC_LOGE(mDisplayIdentifier,
                     "setupCommitFromDisplayConfig failed, "
                     "for 8K last config[%zu], chId(%d)",
                     virtualPlaneIndex, virtualOTFInfo.lastHalf.channelId);
            return ret;
        }
    }

    /* Disable unused plane */
    disablePlanes(mDrmReq, planeEnableInfo);

    mColorRequest.apply(mDrmReq, mDrmCrtc);

    uint32_t flags = dpuData.enable_readback ? (DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_ATOMIC_ALLOW_MODESET) : (DRM_MODE_ATOMIC_NONBLOCK);

    if ((ret = mDrmReq.commit(flags, true)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in deliverWinConfigData()\n",
                 __func__, ret);
        return ret;
    }

    if (dpuData.enable_standalone_writeback) {
        dpuData.present_fence = dpuData.standalone_writeback_info.acq_fence;
        close(out_fences[mDrmCrtc->pipe()]);
    } else {
        dpuData.present_fence = (int)out_fences[mDrmCrtc->pipe()];
    }

    /* Prevent zero fence return */
    if (dpuData.present_fence == 0) {
        ALOGE("%d Getting present_fence = 0!!", __LINE__);
        dpuData.present_fence = -1;
    }

    /*
     * [HACK] dup present_fence for each layer's release fence
     * Do not use hwc_dup because hwc_dup increase usage count of fence treacer
     * Usage count of this fence is incresed by ExynosDisplay::deliverWinConfigData()
     */
    for (auto &display_config : dpuData.configs) {
        if ((display_config.state != display_config.WIN_STATE_BUFFER) &&
            (display_config.state != display_config.WIN_STATE_CURSOR))
            continue;

        if (dpuData.enable_standalone_writeback)
            display_config.rel_fence = dup(dpuData.standalone_writeback_info.acq_fence);
        else
            display_config.rel_fence = dup((int)out_fences[mDrmCrtc->pipe()]);
        /* acq_fence is used by only firstHalf */
    }

    if (mDesiredModeState.needs_modeset) {
        HDEBUGLOGD(eDebugDisplayConfig, "%s:: mActiveModeState is updated to mDesiredModeState(%d -> %d)",
                   __func__, mActiveModeState.mode.id(), mDesiredModeState.mode.id());
        mDesiredModeState.apply(mActiveModeState, mDrmReq);
    }

    return NO_ERROR;
}

bool ExynosDisplayDrmInterface::getVirtual8KOtfInfo(
    exynos_win_config_data &config, virtual8KOTFInfo &virtualOtfInfo) {
    if (!config.assignedMPP->isVirtual8KOtf() ||
        ((config.state != config.WIN_STATE_BUFFER) &&
         (config.state != config.WIN_STATE_COLOR)))
        return false;

    int32_t padding = 0;
    if ((config.transform == HAL_TRANSFORM_ROT_90) ||
        (config.transform == HAL_TRANSFORM_ROT_270)) {
        if ((config.src.h != config.dst.w) ||
            (config.src.w != config.dst.h)) {
            padding = 4;
        }
    } else {
        if ((config.src.w != config.dst.w) ||
            (config.src.h != config.dst.h)) {
            padding = 4;
        }
    }

    virtual8KOTFHalfInfo &firstHalf = virtualOtfInfo.firstHalf;
    virtual8KOTFHalfInfo &lastHalf = virtualOtfInfo.lastHalf;
    ExynosMPP *subMPP0 = nullptr;
    ExynosMPP *subMPP1 = nullptr;
    if (!config.assignedMPP->getSubMPPs(&subMPP0, &subMPP1))
        return false;

    if (((firstHalf.channelId = subMPP0->mChId) < 0) ||
        ((lastHalf.channelId = subMPP1->mChId) < 0)) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to get sub channel id(%d, %d)",
                 __func__, firstHalf.channelId, lastHalf.channelId);
        return false;
    }

    firstHalf.config = config;
    if (firstHalf.config.transform == HAL_TRANSFORM_ROT_90) {
        firstHalf.config.src.y = firstHalf.config.src.y + firstHalf.config.src.h / 2 - padding;
        firstHalf.config.src.h = firstHalf.config.src.h / 2 + padding;
        firstHalf.config.dst.w = firstHalf.config.dst.w / 2;
        firstHalf.config.split = EXYNOS_SPLIT_BOTTOM;
    } else if (firstHalf.config.transform == HAL_TRANSFORM_ROT_180) {
        firstHalf.config.src.x = firstHalf.config.src.x + firstHalf.config.src.w / 2 - padding;
        firstHalf.config.src.w = firstHalf.config.src.w / 2 + padding;
        firstHalf.config.dst.w = firstHalf.config.dst.w / 2;
        firstHalf.config.split = EXYNOS_SPLIT_RIGHT;
    } else if (firstHalf.config.transform == HAL_TRANSFORM_ROT_270) {
        firstHalf.config.src.h = firstHalf.config.src.h / 2 + padding;
        firstHalf.config.dst.w = firstHalf.config.dst.w / 2;
        firstHalf.config.split = EXYNOS_SPLIT_TOP;
    } else if (firstHalf.config.transform == HAL_TRANSFORM_FLIP_H) {
        firstHalf.config.src.w = firstHalf.config.src.w / 2 + padding;
        firstHalf.config.dst.x = firstHalf.config.dst.x + firstHalf.config.dst.w / 2;
        firstHalf.config.dst.w = firstHalf.config.dst.w / 2;
        firstHalf.config.split = EXYNOS_SPLIT_LEFT;
    } else {
        firstHalf.config.src.w = firstHalf.config.src.w / 2 + padding;
        firstHalf.config.dst.w = firstHalf.config.dst.w / 2;
        firstHalf.config.split = EXYNOS_SPLIT_LEFT;
    }

    lastHalf.config = config;
    /* With Rotation */
    if (lastHalf.config.transform == HAL_TRANSFORM_ROT_90) {
        lastHalf.config.src.h = lastHalf.config.src.h / 2 + padding;
        lastHalf.config.dst.x = lastHalf.config.dst.x + lastHalf.config.dst.w / 2;
        lastHalf.config.dst.w = lastHalf.config.dst.w / 2;
        lastHalf.config.split = EXYNOS_SPLIT_TOP;
    } else if (lastHalf.config.transform == HAL_TRANSFORM_ROT_180) {
        lastHalf.config.src.w = lastHalf.config.src.w / 2 + padding;
        lastHalf.config.dst.x = lastHalf.config.dst.x + lastHalf.config.dst.w / 2;
        lastHalf.config.dst.w = lastHalf.config.dst.w / 2;
        lastHalf.config.split = EXYNOS_SPLIT_LEFT;
    } else if (lastHalf.config.transform == HAL_TRANSFORM_ROT_270) {
        lastHalf.config.src.y = lastHalf.config.src.y + lastHalf.config.src.h / 2 - padding;
        lastHalf.config.src.h = lastHalf.config.src.h / 2 + padding;
        lastHalf.config.dst.x = lastHalf.config.dst.x + lastHalf.config.dst.w / 2;
        lastHalf.config.dst.w = lastHalf.config.dst.w / 2;
        lastHalf.config.split = EXYNOS_SPLIT_BOTTOM;
    } else if (lastHalf.config.transform == HAL_TRANSFORM_FLIP_H) {
        lastHalf.config.src.x = lastHalf.config.src.x + lastHalf.config.src.w / 2 - padding;
        lastHalf.config.src.w = lastHalf.config.src.w / 2 + padding;
        lastHalf.config.dst.w = lastHalf.config.dst.w / 2;
        lastHalf.config.split = EXYNOS_SPLIT_RIGHT;
    } else {
        lastHalf.config.src.x = lastHalf.config.src.x + lastHalf.config.src.w / 2 - padding;
        lastHalf.config.src.w = lastHalf.config.src.w / 2 + padding;
        lastHalf.config.dst.x = lastHalf.config.dst.x + lastHalf.config.dst.w / 2;
        lastHalf.config.dst.w = lastHalf.config.dst.w / 2;
        lastHalf.config.split = EXYNOS_SPLIT_RIGHT;
    }

    /* acq_fence is used by only firstHalf */
    lastHalf.config.acq_fence = -1;

    return true;
}

int32_t ExynosDisplayDrmInterface::clearDisplay() {
    int ret = NO_ERROR;
    DrmModeAtomicReq drmReq(this);

    /* Disable all planes */
    disablePlanes(drmReq);

    ret = drmReq.commit(DRM_MODE_ATOMIC_ALLOW_MODESET, true);
    if (ret) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in clearDisplay()\n",
                 __func__, ret);
        return ret;
    }

    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::disableSelfRefresh(uint32_t disable) {
    int ret = NO_ERROR;
    DrmModeAtomicReq drmReq(this);

    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->dsr_status_property(), !disable) < 0)) {
        return ret;
    }

    uint32_t flags = 0;
    if ((ret = drmReq.commit(flags, true)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Failed to commit pset ret=%d in disableSelfRefresh()\n",
                 __func__, ret);
        return ret;
    }

    return ret;
}

int32_t ExynosDisplayDrmInterface::setForcePanic() {
    if (exynosHWCControl.forcePanic == 0)
        return NO_ERROR;

    usleep(20000000);

    FILE *forcePanicFd = fopen(HWC_FORCE_PANIC_PATH, "w");
    if (forcePanicFd == NULL) {
        ALOGW("%s:: Failed to open fd", __func__);
        return -1;
    }

    int val = 'Y';
    fwrite(&val, sizeof(int), 1, forcePanicFd);
    fclose(forcePanicFd);

    return 0;
}

uint32_t ExynosDisplayDrmInterface::getMaxWindowNum() {
    return mDrmDevice->planes().size();
}

ExynosDisplayDrmInterface::DrmModeAtomicReq::DrmModeAtomicReq(ExynosDisplayDrmInterface *displayInterface) {
    init(displayInterface);
}

void ExynosDisplayDrmInterface::DrmModeAtomicReq::init(ExynosDisplayDrmInterface *displayInterface) {
    mDrmDisplayInterface = displayInterface;
    mPset = drmModeAtomicAlloc();
}

ExynosDisplayDrmInterface::DrmModeAtomicReq::~DrmModeAtomicReq() {
    reset();
    if (mPset)
        drmModeAtomicFree(mPset);
}

void ExynosDisplayDrmInterface::DrmModeAtomicReq::reset() {
    if (mError != 0) {
        android::String8 result;
        result.appendFormat("atomic commit error\n");
        dumpAtomicCommitInfo(result);
        HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier, "%s", result.c_str());
        mError = 0;
    }

    if (destroyOldBlobs() != NO_ERROR)
        HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier, "destroy blob error");

    drmModeAtomicSetCursor(mPset, 0);
}

int32_t ExynosDisplayDrmInterface::DrmModeAtomicReq::atomicAddProperty(
    const uint32_t id,
    const DrmProperty &property,
    uint64_t value, bool optional) {
    if (!optional && !property.id()) {
        HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier, "%s:: %s property id(%d) for id(%d) is not available",
                 __func__, property.name().c_str(), property.id(), id);
        return -EINVAL;
    }

    if (property.id()) {
        int ret = drmModeAtomicAddProperty(mPset, id,
                                           property.id(), value);
        if (ret < 0) {
            HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier, "%s:: Failed to add property %d(%s) for id(%d), ret(%d)",
                     __func__, property.id(), property.name().c_str(), id, ret);
            return ret;
        }
    }

    return NO_ERROR;
}

String8 &ExynosDisplayDrmInterface::DrmModeAtomicReq::dumpAtomicCommitInfo(
    String8 &result, bool debugPrint) {
    /* print log only if eDebugDisplayInterfaceConfig flag is set when debugPrint is true */
    if (debugPrint &&
        (hwcCheckDebugMessages(eDebugDisplayInterfaceConfig) == false))
        return result;

    for (int i = 0; i < drmModeAtomicGetCursor(mPset); i++) {
        const DrmProperty *property = NULL;
        String8 objectName;
        /* Check crtc properties */
        if (mPset->items[i].object_id == mDrmDisplayInterface->mDrmCrtc->id()) {
            for (auto property_ptr : mDrmDisplayInterface->mDrmCrtc->properties()) {
                if (mPset->items[i].property_id == property_ptr->id()) {
                    property = property_ptr;
                    objectName.appendFormat("Crtc");
                    break;
                }
            }
            if (property == NULL) {
                HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier,
                         "%s:: object id is crtc but there is no matched property",
                         __func__);
            }
        } else if (mPset->items[i].object_id == mDrmDisplayInterface->mDrmConnector->id()) {
            for (auto property_ptr : mDrmDisplayInterface->mDrmConnector->properties()) {
                if (mPset->items[i].property_id == property_ptr->id()) {
                    property = property_ptr;
                    objectName.appendFormat("Connector");
                    break;
                }
            }
            if (property == NULL) {
                HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier,
                         "%s:: object id is connector but there is no matched property",
                         __func__);
            }
        } else {
            uint32_t channelId = 0;
            for (auto &plane : mDrmDisplayInterface->mDrmDevice->planes()) {
                if (mPset->items[i].object_id == plane->id()) {
                    for (auto property_ptr : plane->properties()) {
                        if (mPset->items[i].property_id == property_ptr->id()) {
                            property = property_ptr;
                            objectName.appendFormat("Plane[%d]", channelId);
                            break;
                        }
                    }
                    if (property == NULL) {
                        HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier,
                                 "%s:: object id is plane but there is no matched property",
                                 __func__);
                    }
                }
                channelId++;
            }
        }
        if (property == NULL) {
            HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier,
                     "%s:: Fail to get property[%d] (object_id: %d, property_id: %d, value: %" PRId64 ")",
                     __func__, i, mPset->items[i].object_id, mPset->items[i].property_id,
                     mPset->items[i].value);
            continue;
        }

        if (debugPrint)
            ALOGD("property[%d] %s object_id: %d, property_id: %d, name: %s,  value: %" PRId64 ")\n",
                  i, objectName.c_str(), mPset->items[i].object_id, mPset->items[i].property_id, property->name().c_str(), mPset->items[i].value);
        else
            result.appendFormat("property[%d] %s object_id: %d, property_id: %d, name: %s,  value: %" PRId64 ")\n",
                                i, objectName.c_str(), mPset->items[i].object_id, mPset->items[i].property_id, property->name().c_str(), mPset->items[i].value);
    }
    return result;
}

int ExynosDisplayDrmInterface::DrmModeAtomicReq::commit(uint32_t flags, bool loggingForDebug) {
    android::String8 result;
    int ret = drmModeAtomicCommit(mDrmDisplayInterface->mDrmDevice->fd(),
                                  mPset, flags, mDrmDisplayInterface->mDrmDevice);
    if (loggingForDebug)
        dumpAtomicCommitInfo(result, true);
    if (ret < 0) {
        HWC_LOGE(mDrmDisplayInterface->mDisplayIdentifier, "commit error");
        setError(ret);
    }

    return ret;
}

std::tuple<uint64_t, int> ExynosDisplayDrmInterface::halToDrmEnum(
    const int32_t halData, const DrmPropertyMap &drmEnums) {
    auto it = drmEnums.find(halData);
    if (it != drmEnums.end()) {
        return std::make_tuple(it->second, 0);
    } else {
        HWC_LOGE_NODISP("%s::Failed to find enum(%d)", __func__, halData);
        return std::make_tuple(0, -EINVAL);
    }
}

int32_t ExynosDisplayDrmInterface::getReadbackBufferAttributes(
    int32_t * /*android_pixel_format_t*/ outFormat,
    int32_t * /*android_dataspace_t*/ outDataspace) {
    DrmConnector *writeback_conn = mWritebackInfo.getWritebackConnector();
    if (writeback_conn == NULL) {
        ALOGE("%s: There is no writeback connection", __func__);
        return -EINVAL;
    }
    mWritebackInfo.pickFormatDataspace();
    if (mWritebackInfo.mWritebackFormat ==
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        ALOGE("writeback format(%d) is not valid",
              mWritebackInfo.mWritebackFormat);
        return -EINVAL;
    }
    *outFormat = mWritebackInfo.mWritebackFormat;
    *outDataspace = HAL_DATASPACE_UNKNOWN;
    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::setupConcurrentWritebackCommit(
    exynos_dpu_data &dpuData, DrmModeAtomicReq &drmReq) {
    int ret = NO_ERROR;
    DrmConnector *writeback_conn = mWritebackInfo.getWritebackConnector();
    if (writeback_conn == NULL) {
        HWC_LOGE(mDisplayIdentifier, "%s: There is no writeback connection",
                 __func__);
        return -EINVAL;
    }
    if (writeback_conn->writeback_fb_id().id() == 0 ||
        writeback_conn->writeback_out_fence().id() == 0) {
        HWC_LOGE(mDisplayIdentifier, "%s: Writeback properties don't exit",
                 __func__);
        return -EINVAL;
    }

    uint32_t writeback_fb_id = 0;
    ExynosGraphicBufferMeta gmeta(dpuData.readback_info.handle);

    exynos_win_config_data writeback_config;
    writeback_config.state = exynos_win_config_data::WIN_STATE_BUFFER;
    writeback_config.format = mWritebackInfo.mWritebackFormat;
    writeback_config.src = {0, 0, mDisplayInfo.xres, mDisplayInfo.yres,
                            gmeta.stride, gmeta.vstride};
    writeback_config.dst = {0, 0, mDisplayInfo.xres, mDisplayInfo.yres,
                            gmeta.stride, gmeta.vstride};
    writeback_config.fd_idma[0] = gmeta.fd;
    writeback_config.fd_idma[1] = gmeta.fd1;
    writeback_config.fd_idma[2] = gmeta.fd2;
    writeback_config.buffer_id =
        ExynosGraphicBufferMeta::get_buffer_id(dpuData.readback_info.handle);

    if ((ret = getBufferId(writeback_config, writeback_fb_id, kUseBufferCaching)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_fb_id(),
                                        writeback_fb_id)) < 0)
        return ret;

    if (dpuData.readback_info.acq_fence >= 0) {
        dpuData.readback_info.acq_fence =
            mFenceTracer.fence_close(dpuData.readback_info.acq_fence,
                                     mDisplayIdentifier, FENCE_TYPE_READBACK_ACQUIRE, FENCE_IP_DPP,
                                     "display::setReadbackBufferAcqFence: acq_fence");
        HWC_LOGE(mDisplayIdentifier, "previous readback out fence is not delivered to framework");
    }

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_out_fence(),
                                        (uint64_t)&dpuData.readback_info.acq_fence)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->crtc_id_property(),
                                        mDrmCrtc->id())) < 0)
        return ret;

    return NO_ERROR;
}

int32_t ExynosDisplayDrmInterface::setupStandaloneWritebackCommit(
    exynos_dpu_data &dpuData, DrmModeAtomicReq &drmReq) {
    int ret = NO_ERROR;
    int wfdMode = mDisplayInfo.isWFDState;

    DrmConnector *writeback_conn = mWritebackInfo.getWritebackConnector();
    if (writeback_conn == NULL) {
        HWC_LOGE(mDisplayIdentifier, "%s: There is no writeback connection",
                 __func__);
        return -EINVAL;
    }
    if (writeback_conn->writeback_fb_id().id() == 0 ||
        writeback_conn->writeback_out_fence().id() == 0) {
        HWC_LOGE(mDisplayIdentifier, "%s: Writeback properties don't exit",
                 __func__);
        return -EINVAL;
    }

    uint32_t writeback_fb_id = 0;
    ExynosGraphicBufferMeta gmeta(dpuData.standalone_writeback_info.handle);

    exynos_win_config_data writeback_config;
    writeback_config.state = exynos_win_config_data::WIN_STATE_BUFFER;
    writeback_config.format = gmeta.format;
    writeback_config.src = {0, 0, mDisplayInfo.xres, mDisplayInfo.yres,
                            gmeta.stride, gmeta.vstride};
    writeback_config.dst = {0, 0, mDisplayInfo.xres, mDisplayInfo.yres,
                            gmeta.stride, gmeta.vstride};
    if (wfdMode == LLWFD) {
        writeback_config.src.f_w = writeback_config.dst.f_w = mDisplayInfo.xres;
        writeback_config.src.f_h = writeback_config.dst.f_h = mDisplayInfo.yres;
    }
    writeback_config.fd_idma[0] = gmeta.fd;
    writeback_config.fd_idma[1] = gmeta.fd1;
    writeback_config.fd_idma[2] = gmeta.fd2;
    writeback_config.buffer_id =
        ExynosGraphicBufferMeta::get_buffer_id(dpuData.standalone_writeback_info.handle);
    writeback_config.rel_fence = dpuData.standalone_writeback_info.rel_fence;
    writeback_config.protection = getDrmMode(gmeta.producer_usage) == SECURE_DRM ? 1 : 0;
    if ((ret = getBufferId(writeback_config, writeback_fb_id, kUseBufferCaching)) < 0)
        return ret;

    uint64_t drmEnum = 0;
    std::tie(drmEnum, ret) =
        halToDrmEnum(HAL_DATASPACE_STANDARD_BT709, mStandardEnums);
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "Fail to convert standard(%d)",
                 HAL_DATASPACE_STANDARD_BT709);
        return ret;
    }
    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_standard(),
                                        drmEnum, true)) < 0)
        return ret;

    std::tie(drmEnum, ret) =
        halToDrmEnum(HAL_DATASPACE_RANGE_LIMITED, mRangeEnums);
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "Fail to convert range(%d)",
                 HAL_DATASPACE_RANGE_LIMITED);
        return ret;
    }
    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_range(), drmEnum, true)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_fb_id(),
                                        writeback_fb_id)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->writeback_out_fence(),
                                        (uint64_t)&dpuData.standalone_writeback_info.acq_fence)) < 0)
        return ret;

    if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                        writeback_conn->crtc_id_property(),
                                        mDrmCrtc->id())) < 0)
        return ret;

    if (writeback_conn->writeback_use_repeater_buffer().id()) {
        if ((ret = drmReq.atomicAddProperty(writeback_conn->id(),
                                            writeback_conn->writeback_use_repeater_buffer(),
                                            (uint64_t)(wfdMode == LLWFD ? 1 : 0))) < 0)
            return ret;
    }

    return NO_ERROR;
}

void ExynosDisplayDrmInterface::DrmWritebackInfo::init(DrmDevice *drmDevice, uint32_t displayId) {
    mDrmDevice = drmDevice;
    mWritebackConnector = mDrmDevice->AvailableWritebackConnector(displayId);
    if (mWritebackConnector == NULL) {
        ALOGI("writeback is not supported");
        return;
    }
    if (mWritebackConnector->writeback_fb_id().id() == 0 ||
        mWritebackConnector->writeback_out_fence().id() == 0) {
        ALOGE("%s: Writeback properties don't exit", __func__);
        mWritebackConnector = NULL;
        return;
    }

    if (mWritebackConnector->writeback_pixel_formats().id()) {
        int32_t ret = NO_ERROR;
        uint64_t blobId;
        std::tie(ret, blobId) = mWritebackConnector->writeback_pixel_formats().value();
        if (ret) {
            ALOGE("Fail to get blob id for writeback_pixel_formats");
            return;
        }
        drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(mDrmDevice->fd(), blobId);
        if (!blob) {
            ALOGE("Fail to get blob for writeback_pixel_formats(%" PRId64 ")", blobId);
            return;
        }
        uint32_t formatNum = (blob->length) / sizeof(uint32_t);
        uint32_t *formats = (uint32_t *)blob->data;
        for (uint32_t i = 0; i < formatNum; i++) {
            int halFormat = drmFormatToHalFormat(formats[i]);
            ALOGD("supported writeback format[%d] %4.4s, %d", i, (char *)&formats[i], halFormat);
            if (halFormat != HAL_PIXEL_FORMAT_EXYNOS_UNDEFINED)
                mSupportedFormats.push_back(halFormat);
        }
        drmModeFreePropertyBlob(blob);
    }
}

void ExynosDisplayDrmInterface::DrmWritebackInfo::pickFormatDataspace() {
    if (!mSupportedFormats.empty())
        mWritebackFormat = mSupportedFormats[0];
    auto it = std::find(mSupportedFormats.begin(),
                        mSupportedFormats.end(), PREFERRED_READBACK_FORMAT);
    if (it != mSupportedFormats.end())
        mWritebackFormat = *it;
}

int32_t ExynosDisplayDrmInterface::getDisplayIdentificationData(
    uint8_t *outPort, uint32_t *outDataSize, uint8_t *outData) {
    if ((mDrmDevice == nullptr) || (mDrmConnector == nullptr)) {
        ALOGE("%s: display(%s) mDrmDevice(%p), mDrmConnector(%p)",
              __func__, mDisplayIdentifier.name.c_str(),
              mDrmDevice, mDrmConnector);
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (mDrmConnector->UpdateEdid() < 0) {
        ALOGE("UpdateEdid fail");
    }

    if (mDrmConnector->edid_property().id() == 0) {
        ALOGD("%s: edid_property is not supported",
              mDisplayIdentifier.name.c_str());
        return HWC2_ERROR_UNSUPPORTED;
    }

    drmModePropertyBlobPtr blob;
    int ret;
    uint64_t blobId;

    std::tie(ret, blobId) = mDrmConnector->edid_property().value();
    if (ret) {
        ALOGE("Failed to get edid property value.");
        return HWC2_ERROR_UNSUPPORTED;
    }
    if (blobId == 0) {
        ALOGD("%s: edid_property is supported but blob is not valid",
              mDisplayIdentifier.name.c_str());
        return HWC2_ERROR_UNSUPPORTED;
    }

    blob = drmModeGetPropertyBlob(mDrmDevice->fd(), blobId);
    if (blob == nullptr) {
        ALOGD("%s: Failed to get blob",
              mDisplayIdentifier.name.c_str());
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (outData) {
        *outDataSize = std::min(*outDataSize, blob->length);
        memcpy(outData, blob->data, *outDataSize);
    } else {
        *outDataSize = blob->length;
    }
    *outPort = mDrmConnector->id();
    drmModeFreePropertyBlob(blob);

    return HWC2_ERROR_NONE;
}

void ExynosDisplayDrmInterface::setDeviceToDisplayInterface(
    const struct DeviceToDisplayInterface &initData) {
    if (initData.exynosMPPsForChannel.size() != mDrmDevice->planes().size()) {
        ALOGI("exynosMPPsForChannel.size(%zu), planes.size(%zu) are different",
              initData.exynosMPPsForChannel.size(),
              mDrmDevice->planes().size());
    }
    if (initData.exynosMPPsForChannel.size() < mDrmDevice->planes().size())
        return;

    for (uint32_t i = 0; i < mDrmDevice->planes().size(); i++) {
        auto &plane = mDrmDevice->planes().at(i);
        uint32_t plane_id = plane->id();
        if (initData.exynosMPPsForChannel[i] == nullptr) {
            ALOGE("There is no ExynosMPP for channel(%d), plane(%d)", i, plane_id);
            continue;
        }
        mExynosMPPsForPlane[plane_id] = initData.exynosMPPsForChannel[i];
    }
}

bool ExynosDisplayDrmInterface::readHotplugStatus() {
    if (mDrmConnector == nullptr) {
        return false;
    }

    uint32_t numConfigs;
    std::map<uint32_t, displayConfigs_t> temp;
    getDisplayConfigs(&numConfigs, NULL, temp);

    if (mDrmConnector->state() == DRM_MODE_CONNECTED)
        return true;
    else if (mDrmConnector->state() == DRM_MODE_DISCONNECTED)
        return false;

    return false;
}

bool ExynosDisplayDrmInterface::updateHdrSinkInfo() {
    if (mDisplayIdentifier.type != HWC_DISPLAY_EXTERNAL) {
        return false;
    }

    if (mDrmConnector->UpdateHdrInfo() < 0) {
        ALOGE("UpdateHdrInfo fail");
    }

    int ret = 0;
    const DrmProperty &prop_hdr_sink_connected = mDrmConnector->hdr_sink_connected();
    uint64_t hdr_sink_connected = 0;
    std::tie(ret, hdr_sink_connected) = prop_hdr_sink_connected.value();
    if (ret < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: there is no hdr_sink_connected (ret = %d)", __func__, ret);
        hdr_sink_connected = 0;
    }

    return mIsHdrSink = hdr_sink_connected == 0 ? false : true;
}

void ExynosDisplayDrmInterface::onDisplayRemoved() {
    mFBManager.onDisplayRemoved(mDisplayIdentifier.type);
}

int32_t ExynosDisplayDrmInterface::setWorkingVsyncPeriodProp(DrmModeAtomicReq &drmReq) {
    if ((mDrmCrtc == nullptr) || !mDrmCrtc->bts_fps_property().id()) {
        HWC_LOGE(mDisplayIdentifier, "bts_fps is not supported");
        return NO_ERROR;
    }
    int ret = NO_ERROR;
    if ((ret = drmReq.atomicAddProperty(mDrmCrtc->id(),
                                        mDrmCrtc->bts_fps_property(),
                                        (uint64_t)&mWorkingVsyncPeriod)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: Fail to add bts_fps_property",
                 __func__);
        return ret;
    }
    return ret;
}
