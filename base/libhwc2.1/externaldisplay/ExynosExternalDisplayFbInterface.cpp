/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "ExynosDisplayFbInterface.h"
#include "ExynosExternalDisplayFbInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"
#include "displayport_for_hwc.h"
#include <math.h>

#include <linux/fb.h>
#include "ExynosGraphicBuffer.h"

using namespace android;
using vendor::graphics::ExynosGraphicBufferMeta;

constexpr float DISPLAY_LUMINANCE_UNIT = 10000;
constexpr float CONSTANT_FOR_DP_MIN_LUMINANCE = 255.0f * 255.0f / 100.0f;

//////////////////////////////////////////////////// ExynosExternalDisplayFbInterface //////////////////////////////////////////////////////////////////

bool is_same_dv_timings(const struct v4l2_dv_timings *t1,
                        const struct v4l2_dv_timings *t2) {
    if (t1->type == t2->type &&
        t1->bt.width == t2->bt.width &&
        t1->bt.height == t2->bt.height &&
        t1->bt.interlaced == t2->bt.interlaced &&
        t1->bt.polarities == t2->bt.polarities &&
        t1->bt.pixelclock == t2->bt.pixelclock &&
        t1->bt.hfrontporch == t2->bt.hfrontporch &&
        t1->bt.vfrontporch == t2->bt.vfrontporch &&
        t1->bt.vsync == t2->bt.vsync &&
        t1->bt.vbackporch == t2->bt.vbackporch &&
        (!t1->bt.interlaced ||
         (t1->bt.il_vfrontporch == t2->bt.il_vfrontporch &&
          t1->bt.il_vsync == t2->bt.il_vsync &&
          t1->bt.il_vbackporch == t2->bt.il_vbackporch)))
        return true;
    return false;
}

#if 0
int ExynosExternalDisplay::getDVTimingsIndex(int preset)
{
    for (int i = 0; i < SUPPORTED_DV_TIMINGS_NUM; i++) {
        if (preset == preset_index_mappings[i].preset)
            return preset_index_mappings[i].dv_timings_index;
    }
    return -1;
}
#endif

ExynosExternalDisplayFbInterface::ExynosExternalDisplayFbInterface()
    : ExynosDisplayFbInterface() {
    *mDVTimings = {};
    cleanConfigurations();
}

void ExynosExternalDisplayFbInterface::init(const DisplayIdentifier &display,
                                            void *__unused deviceData, const size_t __unused deviceDataSize) {
    mDisplayIdentifier = display;

    mDisplayFd = open(display.deconNodeName.c_str(), O_RDWR);
    if (mDisplayFd < 0)
        ALOGE("%s:: %s failed to open framebuffer", __func__,
              mDisplayIdentifier.name.c_str());
    setVsyncFd();

    *mDVTimings = {};
}

int32_t ExynosExternalDisplayFbInterface::getDisplayConfigs(uint32_t *outNumConfigs,
                                                            hwc2_config_t *outConfigs, std::map<uint32_t, displayConfigs_t> &displayConfigs) {
    int ret = 0;
    exynos_displayport_data dp_data = {};

    if (outConfigs != NULL) {
        if (mConfigurations.size() != *outNumConfigs) {
            HWC_LOGE(mDisplayIdentifier, "%s outNumConfigs(%d) is different with the number of configurations(%zu)",
                     mDisplayIdentifier.name.c_str(), *outNumConfigs, mConfigurations.size());
            return HWC2_ERROR_BAD_PARAMETER;
        }

        for (size_t index = 0; index < *outNumConfigs; index++) {
            outConfigs[index] = mConfigurations[index];
        }

        dp_data.timings = mDVTimings[outConfigs[0]];
        dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_PRESET;
        if (ioctl(mDisplayFd, EXYNOS_SET_DISPLAYPORT_CONFIG, &dp_data) < 0) {
            HWC_LOGE(mDisplayIdentifier, "%s fail to send selected config data, %d",
                     mDisplayIdentifier.name.c_str(), errno);
            return HWC2_ERROR_BAD_PARAMETER;
        }

        updateDisplayConfigs(displayConfigs);

        dumpDisplayConfigs();

        return HWC2_ERROR_NONE;
    }

    *mDVTimings = {};
    cleanConfigurations();

    /* configs store the index of mConfigurations */
    dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_ENUM_PRESET;
    for (size_t index = 0; index < SUPPORTED_DV_TIMINGS_NUM; index++) {
        dp_data.etimings.index = index;
        ret = ioctl(mDisplayFd, EXYNOS_GET_DISPLAYPORT_CONFIG, &dp_data);
        if (ret < 0) {
            if (errno == EINVAL) {
                HDEBUGLOGD(eDebugExternalDisplay, "%s:: Unmatched config index %zu", __func__, index);
                continue;
            } else if (errno == E2BIG) {
                HDEBUGLOGD(eDebugExternalDisplay, "%s:: Total configurations %zu", __func__, index);
                break;
            }
            HWC_LOGE(mDisplayIdentifier, "%s: enum_dv_timings error, %d", __func__, errno);
            return HWC2_ERROR_BAD_PARAMETER;
        }

        mDVTimings[index] = dp_data.etimings.timings;
        mConfigurations.push_back(index);
    }

    if (mConfigurations.size() == 0) {
        HWC_LOGE(mDisplayIdentifier, "%s do not receivce any configuration info",
                 mDisplayIdentifier.name.c_str());
        return HWC2_ERROR_BAD_PARAMETER;
    }

    int config = 0;
    v4l2_dv_timings temp_dv_timings = mDVTimings[mConfigurations[mConfigurations.size() - 1]];
    for (config = 0; config < (int)mConfigurations[mConfigurations.size() - 1]; config++) {
        if (mDVTimings[config].bt.width != 0) {
            mDVTimings[mConfigurations[mConfigurations.size() - 1]] = mDVTimings[config];
            break;
        }
    }

    mDVTimings[config] = temp_dv_timings;

    *outNumConfigs = mConfigurations.size();

    return HWC2_ERROR_NONE;
}

void ExynosExternalDisplayFbInterface::cleanConfigurations() {
    mConfigurations.clear();
}

void ExynosExternalDisplayFbInterface::dumpDisplayConfigs() {
    for (size_t i = 0; i < mConfigurations.size(); i++) {
        unsigned int dv_timings_index = mConfigurations[i];
        v4l2_dv_timings configuration = mDVTimings[dv_timings_index];
        float refresh_rate = (float)((float)configuration.bt.pixelclock /
                                     ((configuration.bt.width + configuration.bt.hfrontporch + configuration.bt.hsync + configuration.bt.hbackporch) *
                                      (configuration.bt.height + configuration.bt.vfrontporch + configuration.bt.vsync + configuration.bt.vbackporch)));
        uint32_t vsyncPeriod = 1000000000 / refresh_rate;
        ALOGI("%zu : index(%d) type(%d), %d x %d, fps(%f), vsyncPeriod(%d)", i, dv_timings_index, configuration.type, configuration.bt.width,
              configuration.bt.height,
              refresh_rate, vsyncPeriod);
    }
}

void ExynosExternalDisplayFbInterface::updateDisplayConfigs(std::map<uint32_t, displayConfigs_t> &displayConfigs) {
    /* key: (width<<32 | height) */
    std::map<uint64_t, uint32_t> groupIds;
    uint32_t groupId = 0;

    displayConfigs.clear();
    for (size_t i = 0; i < mConfigurations.size(); i++) {
        hwc2_config_t index = mConfigurations[i];
        v4l2_dv_timings dv_timing = mDVTimings[index];
        displayConfigs_t configs;
        configs.vsyncPeriod = calVsyncPeriod(dv_timing);
        configs.width = dv_timing.bt.width;
        configs.height = dv_timing.bt.height;
        /* To Do : xdpi, ydpi */
        configs.Xdpi = UINT32_MAX;
        ;
        configs.Ydpi = UINT32_MAX;

        uint64_t key = ((uint64_t)configs.width << 32) | configs.height;
        auto it = groupIds.find(key);
        if (it != groupIds.end()) {
            configs.groupId = it->second;
        } else {
            groupIds.insert(std::make_pair(key, groupId));
            configs.groupId = groupId;
            groupId++;
        }
        configs.groupId = groupId;

        displayConfigs.insert(std::make_pair(index, configs));
    }
}
int32_t ExynosExternalDisplayFbInterface::calVsyncPeriod(v4l2_dv_timings dv_timing) {
    int32_t result;
    float refreshRate = (float)((float)dv_timing.bt.pixelclock /
                                ((dv_timing.bt.width + dv_timing.bt.hfrontporch + dv_timing.bt.hsync + dv_timing.bt.hbackporch) *
                                 (dv_timing.bt.height + dv_timing.bt.vfrontporch + dv_timing.bt.vsync + dv_timing.bt.vbackporch)));

    result = (1000000000 / refreshRate);
    return result;
}

bool ExynosExternalDisplayFbInterface::updateHdrSinkInfo() {
    exynos_displayport_data dp_data = {};
    dp_data.state = dp_data.EXYNOS_DISPLAYPORT_STATE_HDR_INFO;
    int ret = ioctl(mDisplayFd, EXYNOS_GET_DISPLAYPORT_CONFIG, &dp_data);
    if (ret < 0) {
        ALOGE("%s: EXYNOS_DISPLAYPORT_STATE_HDR_INFO ioctl error, %d", __func__, errno);
    }

    return mIsHdrSink = dp_data.hdr_support == 0 ? false : true;
}

int32_t ExynosExternalDisplayFbInterface::updateHdrCapabilities(std::vector<int32_t> &outTypes,
                                                                float *outMaxLuminance, float *outMaxAverageLuminance, float *outMinLuminance) {
    struct decon_hdr_capabilities_info outInfo = {};
    if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES_NUM, &outInfo) < 0) {
        ALOGE("updateHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES_NUM ioctl failed");
        return -1;
    }

    if (mIsHdrSink) {
        *outMaxLuminance = 50 * pow(2.0, (double)outInfo.max_luminance / 32);
        *outMaxAverageLuminance = 50 * pow(2.0, (double)outInfo.max_average_luminance / 32);
        *outMinLuminance = (*outMaxLuminance) * static_cast<float>(outInfo.min_luminance * outInfo.min_luminance) / CONSTANT_FOR_DP_MIN_LUMINANCE;
    } else {
        *outMaxLuminance = static_cast<float>(outInfo.max_luminance) / DISPLAY_LUMINANCE_UNIT;
        *outMaxAverageLuminance = static_cast<float>(outInfo.max_average_luminance) / DISPLAY_LUMINANCE_UNIT;
        *outMinLuminance = static_cast<float>(outInfo.min_luminance) / DISPLAY_LUMINANCE_UNIT;
    }

    struct decon_hdr_capabilities outData = {};

    if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES, &outData) < 0) {
        ALOGE("updateHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES ioctl Failed");
        return -1;
    }
    outTypes.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(outInfo.out_num); i++) {
        // Save to member variables
        outTypes.push_back(outData.out_types[i]);
        HDEBUGLOGD(eDebugHWC, "HWC2: Types : %d", outData.out_types[i]);
    }
    ALOGI("hdrTypeNum(%d), maxLuminance(%f), maxAverageLuminance(%f), minLuminance(%f)",
          outInfo.out_num, *outMaxLuminance, *outMaxAverageLuminance, *outMinLuminance);
    return 0;
}

bool ExynosExternalDisplayFbInterface::readHotplugStatus() {
    bool hpd_status = 0;
    int sw_fd = open(mUeventNode, O_RDONLY);
    char val;

    if (sw_fd >= 0) {
        if (read(sw_fd, &val, 1) == 1) {
            if (val == '1')
                hpd_status = true;
            else if (val == '0')
                hpd_status = false;
        }
        hwcFdClose(sw_fd);
    }

    return hpd_status;
}
