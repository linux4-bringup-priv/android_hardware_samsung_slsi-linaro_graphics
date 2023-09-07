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
#include "ExynosPrimaryDisplayFbInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"
#include <math.h>

#include <linux/fb.h>
#include "ExynosGraphicBuffer.h"

extern struct exynos_hwc_control exynosHWCControl;
using namespace android;
using vendor::graphics::ExynosGraphicBufferMeta;
#ifndef DECON_READBACK_IDX
#define DECON_READBACK_IDX (-1)
#endif

#ifndef DECON_WRITEBACK_IDX
#define DECON_WRITEBACK_IDX (-1)
#endif

#ifndef DECON_WIN_UPDATE_IDX
#define DECON_WIN_UPDATE_IDX (-1)
#endif

ExynosPrimaryDisplayFbInterface::ExynosPrimaryDisplayFbInterface()
    : ExynosDisplayFbInterface() {
}

void ExynosPrimaryDisplayFbInterface::init(const DisplayIdentifier &display,
                                           void *__unused deviceData, const size_t __unused deviceDataSize) {
    mDisplayIdentifier = display;
    mDisplayFd = open(display.deconNodeName.c_str(), O_RDWR);
    if (mDisplayFd < 0)
        ALOGE("%s:: %s [%s] failed to open framebuffer", __func__, display.name.c_str(), display.deconNodeName.c_str());

    setVsyncFd();
}

int32_t ExynosPrimaryDisplayFbInterface::setPowerMode(int32_t mode) {
    int fb_blank = -1;
    if (mode == HWC_POWER_MODE_DOZE ||
        mode == HWC_POWER_MODE_DOZE_SUSPEND) {
        if (mCurPowerMode != HWC_POWER_MODE_DOZE &&
            mCurPowerMode != HWC_POWER_MODE_OFF &&
            mCurPowerMode != HWC_POWER_MODE_DOZE_SUSPEND) {
            fb_blank = FB_BLANK_POWERDOWN;
        }
    } else if (mode == HWC_POWER_MODE_OFF) {
        fb_blank = FB_BLANK_POWERDOWN;
    } else {
        fb_blank = FB_BLANK_UNBLANK;
    }

    int32_t ret = NO_ERROR;
    if (fb_blank >= 0) {
        if ((ret = ioctl(mDisplayFd, FBIOBLANK, fb_blank)) < 0) {
            ALOGE("FB BLANK ioctl failed errno : %d, ret: %d",
                  errno, ret);
            return HWC2_ERROR_BAD_PARAMETER;
        }
    }

    if ((ret = ioctl(mDisplayFd, S3CFB_POWER_MODE, &mode)) < 0) {
        ALOGE("Need to check S3CFB power mode ioctl : %d, ret: %d",
              errno, ret);
        return HWC2_ERROR_BAD_PARAMETER;
    }
    mCurPowerMode = mode;

    return HWC2_ERROR_NONE;
}

static const uint32_t kMaxPanelModeCnt = 100;
void ExynosPrimaryDisplayFbInterface::getDisplayHWInfo(uint32_t &xres, uint32_t &yres,
                                                       int &psrMode,
                                                       std::vector<ResolutionInfo> &resolutionInfo) {
    /* Notify HWC version to DPU driver
     * this will make decision fence release policy in DPU driver
     */

    struct decon_disp_info disp_info;
    disp_info.ver = HWC_2_0;

    if (ioctl(mDisplayFd, EXYNOS_DISP_INFO, &disp_info) == -1) {
        ALOGI("EXYNOS_DISP_INFO ioctl failed: %s", strerror(errno));
        return;
    } else {
        ALOGI("HWC2: %d, psr_mode : %d", disp_info.ver, disp_info.psr_mode);
    }

    /* Get screen infomation(physical) from Display DD */
    struct fb_var_screeninfo info;

    if (ioctl(mDisplayFd, FBIOGET_VSCREENINFO, &info) == -1) {
        ALOGE("FBIOGET_VSCREENINFO ioctl failed: %s", strerror(errno));
        return;
    }

    /* Put screen infomation(physical) to Display DD */
    if (info.reserved[0] == 0 && info.reserved[1] == 0) {
        info.reserved[0] = info.xres;
        info.reserved[1] = info.yres;

        if (ioctl(mDisplayFd, FBIOPUT_VSCREENINFO, &info) == -1) {
            ALOGE("FBIOPUT_VSCREENINFO ioctl failed: %s", strerror(errno));
            return;
        }
    }

    xres = info.xres;
    yres = info.yres;
    mXres = xres;
    mYres = yres;

    if ((xres == 0) || (yres == 0)) {
        ALOGE("X/Y resoution from DPU driver is zero!!");
        return;
    }

    /* get PSR info */
    char devname[MAX_DEV_NAME + 1];
    devname[MAX_DEV_NAME] = '\0';

    strncpy(devname, VSYNC_DEV_PREFIX, MAX_DEV_NAME);
#if !defined(USES_DUAL_DISPLAY)
    strlcat(devname, PSR_DEV_NAME, MAX_DEV_NAME);
#else
    if (mDisplayIdentifier.index == 0)
        strlcat(devname, PSR_DEV_NAME, MAX_DEV_NAME);
    else
        strlcat(devname, PSR_DEV_NAME_S, MAX_DEV_NAME);
#endif

    char psrDevname[MAX_DEV_NAME + 1] = {0};

    strncpy(psrDevname, devname, strlen(devname) - 8);
    strlcat(psrDevname, "psr_info", MAX_DEV_NAME);
    ALOGI("PSR info devname = %s\n", psrDevname);

    FILE *psrInfoFd = fopen(psrDevname, "r");
    psrMode = PSR_MAX;

    if (psrInfoFd == nullptr) {
        ALOGW("HWC needs to know whether LCD driver is using PSR mode or not\n");
        return;
    }

    char val[4];
    if (fread(&val, 1, 1, psrInfoFd) == 1) {
        psrMode = (0x03 & atoi(val));
    }

    uint32_t panelModeCnt = 1;
    uint32_t sliceXSize = xres;
    uint32_t sliceYSize = yres;
    uint32_t xSize = xres;
    uint32_t ySize = yres;
    uint32_t panelType = PANEL_LEGACY;

    if (fscanf(psrInfoFd, "%u\n", &panelModeCnt) != 1) {
        ALOGE("Fail to read panel mode count");
    } else if (panelModeCnt < kMaxPanelModeCnt) {
        ALOGI("res count : %u", panelModeCnt);
        for (uint32_t i = 0; i < panelModeCnt; i++) {
            if (fscanf(psrInfoFd, "%d\n%d\n%d\n%d\n%d\n", &xSize, &ySize, &sliceXSize, &sliceYSize, &panelType) < 0) {
                ALOGE("Fail to read slice information");
                break;
            } else {
                if ((xSize == xres) && (ySize == yres)) {
                    mDSCHSliceNum = xSize / sliceXSize;
                    mDSCYSliceSize = sliceYSize;
                }
                resolutionInfo.emplace_back(ResolutionInfo(ResolutionSize(xSize, ySize),
                                                           sliceXSize, sliceYSize, panelType));
                ALOGI("mode no. : %d, Width : %d, Height : %d, X_Slice_Size : %d, Y_Slice_Size : %d, Panel type : %d\n", i,
                      resolutionInfo[i].nResolution.w,
                      resolutionInfo[i].nResolution.h,
                      resolutionInfo[i].nDSCXSliceSize,
                      resolutionInfo[i].nDSCYSliceSize,
                      resolutionInfo[i].nPanelType);
            }
        }
    }

    fclose(psrInfoFd);

    mResolutionInfo = resolutionInfo;
    ALOGI("PSR mode   = %d (0: video mode, 1: DP PSR mode, 2: MIPI-DSI command mode)\n",
          psrMode);

    return;
}

int32_t ExynosPrimaryDisplayFbInterface::getVsyncAppliedTime(hwc2_config_t configId, displayConfigs &config, int64_t *actualChangeTime) {
    uint64_t current = systemTime(SYSTEM_TIME_MONOTONIC);
#ifdef USE_NOT_RESERVED_FIELD
    vsync_applied_time_data time_data = {configId, static_cast<uint64_t>(current)};
#else
    vsync_applied_time_data time_data = {configId, static_cast<uint64_t>(current), {0}};
#endif

    /* TODO get from DPU */
    if (ioctl(mDisplayFd, EXYNOS_GET_VSYNC_CHANGE_TIMELINE, &time_data) < 0) {
        ALOGI("EXYNOS_GET_VSYNC_CHANGE_TIMELINE ioctl failed errno : %d %d", errno, mDisplayFd);
        *actualChangeTime = current + config.vsyncPeriod;
        return HWC2_ERROR_UNSUPPORTED;
    }

    *actualChangeTime = time_data.time;

    return NO_ERROR;
}

int32_t ExynosPrimaryDisplayFbInterface::setLowPowerMode(bool suspend) {
    int ret = HWC2_ERROR_NONE;

    if (!isDozeModeAvailable()) {
        return HWC2_ERROR_UNSUPPORTED;
    }

    if (suspend)
        ret = setPowerMode(HWC_POWER_MODE_DOZE_SUSPEND);
    else
        ret = setPowerMode(HWC_POWER_MODE_DOZE);

    return ret;
}

void ExynosPrimaryDisplayFbInterface::updateDSCBlockSize() {
    for (uint32_t i = 0; i < mResolutionInfo.size(); i++) {
        /* find index from resolution */
        if ((mXres == mResolutionInfo[i].nResolution.w) &&
            (mYres == mResolutionInfo[i].nResolution.h)) {
            mDSCHSliceNum = mResolutionInfo[i].nResolution.w / mResolutionInfo[i].nDSCXSliceSize;
            mDSCYSliceSize = mResolutionInfo[i].nDSCYSliceSize;
            HDEBUGLOGD(eDebugWindowUpdate, "window update info : width(%d), height(%d), slice(%d), ySize(%d)",
                       mXres, mYres, mDSCHSliceNum, mDSCYSliceSize);
            break;
        }
    }
}

void ExynosPrimaryDisplayFbInterface::alignDSCBlockSize(hwc_rect &merge_rect) {
    unsigned int blockWidth, blockHeight;

    if (mDSCHSliceNum == 0 || mDSCYSliceSize == 0) {
        ALOGE("DSC size is not valid(%d, %d)", mDSCHSliceNum, mDSCYSliceSize);
        return;
    }

    blockWidth = mXres / mDSCHSliceNum;
    blockHeight = mDSCYSliceSize;

    HDEBUGLOGD(eDebugWindowUpdate, "DSC block size (for align) : %d, %d", blockWidth, blockHeight);
    HDEBUGLOGD(eDebugWindowUpdate, "Partial(origin) : %d, %d, %d, %d",
               merge_rect.left, merge_rect.top, merge_rect.right, merge_rect.bottom);

    if (merge_rect.left % blockWidth != 0)
        merge_rect.left = pixel_align_down(merge_rect.left, blockWidth);
    if (merge_rect.left < 0)
        merge_rect.left = 0;

    if (merge_rect.right % blockWidth != 0)
        merge_rect.right = pixel_align(merge_rect.right, blockWidth);
    if (merge_rect.right > (int32_t)mXres)
        merge_rect.right = mXres;

    if (merge_rect.top % blockHeight != 0)
        merge_rect.top = pixel_align_down(merge_rect.top, blockHeight);
    if (merge_rect.top < 0)
        merge_rect.top = 0;

    if (merge_rect.bottom % blockHeight != 0)
        merge_rect.bottom = pixel_align(merge_rect.bottom, blockHeight);
    if (merge_rect.bottom > (int32_t)mYres)
        merge_rect.bottom = mYres;

    HDEBUGLOGD(eDebugWindowUpdate, "Partial(aligned) : %d, %d, %d, %d",
               merge_rect.left, merge_rect.top, merge_rect.right, merge_rect.bottom);

    return;
}
