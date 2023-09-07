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

#ifndef _EXYNOSEXTERNALDISPLAYFBINTERFACE_H
#define _EXYNOSEXTERNALDISPLAYFBINTERFACE_H

#include "ExynosDisplayInterface.h"
#include <linux/videodev2.h>
#include "videodev2_exynos_displayport.h"
#include "DeconHeader.h"
#include "DeconFbHeader.h"
#include "ExynosHWCTypes.h"

#include <utils/Vector.h>
#define SUPPORTED_DV_TIMINGS_NUM 100
#define DP_RESOLUTION_DEFAULT V4L2_DV_1080P60

struct preset_index_mapping {
    int preset;
    int dv_timings_index;
};

const struct preset_index_mapping preset_index_mappings[SUPPORTED_DV_TIMINGS_NUM] = {
    {V4L2_DV_480P59_94, 0},  // 720X480P59_94
    {V4L2_DV_576P50, 1},
    {V4L2_DV_720P50, 2},
    {V4L2_DV_720P60, 3},
    {V4L2_DV_1080P24, 4},
    {V4L2_DV_1080P25, 5},
    {V4L2_DV_1080P30, 6},
    {V4L2_DV_1080P50, 7},
    {V4L2_DV_1080P60, 8},
    {V4L2_DV_2160P24, 9},
    {V4L2_DV_2160P25, 10},
    {V4L2_DV_2160P30, 11},
    {V4L2_DV_2160P50, 12},
    {V4L2_DV_2160P60, 13},
    {V4L2_DV_2160P24_1, 14},
    {V4L2_DV_2160P25_1, 15},
    {V4L2_DV_2160P30_1, 16},
    {V4L2_DV_2160P50_1, 17},
    {V4L2_DV_2160P60_1, 18},
    {V4L2_DV_2160P59, 19},
    {V4L2_DV_480P60, 20},  // 640X480P60
    {V4L2_DV_1440P59, 21},
    {V4L2_DV_1440P60, 22},
    {V4L2_DV_800P60_RB, 23},  // 1280x800P60_RB
    {V4L2_DV_1024P60, 24},    // 1280x1024P60
    {V4L2_DV_1440P60_1, 25},  // 1920x1440P60
};

class ExynosExternalDisplayFbInterface : public ExynosDisplayFbInterface {
  public:
    String8 mUeventNode;

    ExynosExternalDisplayFbInterface();
    virtual void init(const DisplayIdentifier &display, void *deviceData,
                      const size_t deviceDataSize) override;
    virtual int32_t getDisplayConfigs(uint32_t *outNumConfigs, hwc2_config_t *outConfigs,
                                      std::map<uint32_t, displayConfigs_t> &displayConfigs);
    virtual void dumpDisplayConfigs();
    void updateDisplayConfigs(std::map<uint32_t, displayConfigs_t> &displayConfigs);
    virtual int32_t updateHdrCapabilities(std::vector<int32_t> &outTypes,
                                          float *outMaxLuminance, float *outMaxAverageLuminance, float *outMinLuminance);
    virtual bool readHotplugStatus();
    virtual void updateUeventNodeName(String8 node) { mUeventNode = node.c_str(); };
    virtual bool updateHdrSinkInfo();

  protected:
    int32_t calVsyncPeriod(v4l2_dv_timings dv_timing);
    void cleanConfigurations();

  protected:
    struct v4l2_dv_timings mDVTimings[SUPPORTED_DV_TIMINGS_NUM];
    android::Vector<unsigned int> mConfigurations;
};

#endif
