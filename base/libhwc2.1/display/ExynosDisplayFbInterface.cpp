/*
 * Copyright (C) 2019 The Android Open Source Project
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
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"
#include "displayport_for_hwc.h"
#include <math.h>

#include <linux/fb.h>
#include "ExynosGraphicBuffer.h"

using namespace android;
using vendor::graphics::ExynosGraphicBufferMeta;

extern struct exynos_hwc_control exynosHWCControl;
constexpr float DISPLAY_LUMINANCE_UNIT = 10000;

const size_t NUM_HW_WINDOWS = MAX_DECON_WIN;

#ifndef DECON_READBACK_IDX
#define DECON_READBACK_IDX (-1)
#endif

#ifndef DECON_WRITEBACK_IDX
#define DECON_WRITEBACK_IDX (-1)
#endif

#ifndef DECON_WIN_UPDATE_IDX
#define DECON_WIN_UPDATE_IDX (-1)
#endif
//////////////////////////////////////////////////// ExynosDisplayFbInterface //////////////////////////////////////////////////////////////////
ExynosDisplayFbInterface::ExynosDisplayFbInterface()
    : mDisplayFd(-1), mXres(0), mYres(0), mDSCHSliceNum(0), mDSCYSliceSize(0) {
    mType = INTERFACE_TYPE_FB;
    clearFbWinConfigData(mFbConfigData);
    mEdidData = {};
}

ExynosDisplayFbInterface::~ExynosDisplayFbInterface() {
    if (mDisplayFd >= 0)
        hwcFdClose(mDisplayFd);
    mDisplayFd = -1;
}

void ExynosDisplayFbInterface::init(const DisplayIdentifier &display,
                                    void *__unused deviceData, const size_t __unused deviceDataSize) {
    mDisplayIdentifier = display;
}

int32_t ExynosDisplayFbInterface::setPowerMode(int32_t mode) {
    int fb_blank = 0;
    if (mode == HWC_POWER_MODE_OFF) {
        fb_blank = FB_BLANK_POWERDOWN;
    } else {
        fb_blank = FB_BLANK_UNBLANK;
    }

    ALOGD("%s:: mode(%d), blank(%d)", __func__, mode, fb_blank);
    int32_t ret = NO_ERROR;
    if ((ret = ioctl(mDisplayFd, FBIOBLANK, fb_blank)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "set powermode ioctl failed errno : %d, ret: %d",
                 errno, ret);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::setVsyncEnabled(uint32_t enabled) {
    int32_t ret = NO_ERROR;
    if ((ret = ioctl(mDisplayFd, S3CFB_SET_VSYNC_INT, &enabled)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "set vsync enabled ioctl failed errno: %d, ret: %d",
                 errno, ret);
        return HWC2_ERROR_BAD_PARAMETER;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::getDisplayConfigs(uint32_t *outNumConfigs,
                                                    hwc2_config_t *outConfigs, std::map<uint32_t, displayConfigs_t> &displayConfigs) {
    auto use_legacy = [=](std::map<uint32_t, displayConfigs_t> &displayConfigs) {
        struct fb_var_screeninfo info;
        if (ioctl(mDisplayFd, FBIOGET_VSCREENINFO, &info) != -1) {
            displayConfigs.clear();
            displayConfigs_t configs;
            configs.vsyncPeriod = 1000000000 / 60;
            configs.width = info.xres;
            configs.height = info.yres;
            configs.Xdpi = 1000 * (info.xres * 25.4f) / info.width;
            configs.Ydpi = 1000 * (info.yres * 25.4f) / info.height;
            configs.groupId = 0;
            displayConfigs.insert(std::make_pair(0, configs));
            return HWC2_ERROR_NONE;
        } else {
            ALOGE("FBIOGET_VSCREENINFO ioctl failed: %s", strerror(errno));
            return HWC2_ERROR_UNSUPPORTED;
        }
    };

    if (outConfigs == nullptr) {
        if (ioctl(mDisplayFd, EXYNOS_GET_DISPLAY_MODE_NUM, outNumConfigs) < 0) {
            ALOGI("Not support EXYNOS_GET_DISPLAY_MODE_NUM : %s", strerror(errno));
            *outNumConfigs = 1;
            return HWC2_ERROR_NONE;
        }
    } else if (*outNumConfigs >= 1) {
        displayConfigs.clear();
        for (uint32_t i = 0; i < *outNumConfigs; i++) {
            decon_display_mode mode;
            mode.index = i;
            outConfigs[i] = i;
            if (ioctl(mDisplayFd, EXYNOS_GET_DISPLAY_MODE, &mode) < 0) {
                ALOGI("Not support EXYNOS_GET_DISPLAY_MODE: index(%d) %s", i, strerror(errno));
                return use_legacy(displayConfigs);
            }
            displayConfigs_t configs;
            configs.vsyncPeriod = 1000000000 / mode.fps;
            configs.width = mode.width;
            configs.height = mode.height;
            configs.groupId = mode.group;

            configs.Xdpi = 1000 * (mode.width * 25.4f) / mode.mm_width;
            configs.Ydpi = 1000 * (mode.height * 25.4f) / mode.mm_height;
            ALOGI("Display config : %d, vsync : %d, width : %d, height : %d, xdpi : %d, ydpi : %d, groupId: %d",
                  i, configs.vsyncPeriod, configs.width, configs.height, configs.Xdpi, configs.Ydpi, configs.groupId);
            displayConfigs.insert(std::make_pair(i, configs));
        }
        return HWC2_ERROR_NONE;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::getDPUConfig(hwc2_config_t *config) {
    int32_t _config;
    int ret = ioctl(mDisplayFd, EXYNOS_GET_DISPLAY_CURRENT_MODE, &_config);
    HDEBUGLOGD(eDebugDisplayConfig, "%s, %d", __func__, _config);

    if (ret < 0) {
        ALOGI("EXYNOS_GET_DISPLAY_CURRENT_MODE failed errno : %d, ret: %d", errno, ret);
        *config = 0;
    } else {
        *config = _config;
    }

    return ret;
}

int32_t ExynosDisplayFbInterface::setActiveConfig(hwc2_config_t config,
                                                  displayConfigs_t &displayConfig) {
    int ret = 0;
#if defined(USE_DPU_SET_CONFIG) || defined(USES_SET_DISPLAY_MODE_IOCTL)
#ifdef USE_DPU_SET_CONFIG
    /* Use win_config ioctl */
    struct decon_win_config_data win_data;
    struct decon_win_config *win_config = win_data.config;
    memset(&win_data, 0, sizeof(win_data));

    win_config[DECON_WIN_UPDATE_IDX].state = decon_win_config::DECON_WIN_STATE_MRESOL;
    win_config[DECON_WIN_UPDATE_IDX].dst.f_w = displayConfig.width;
    win_config[DECON_WIN_UPDATE_IDX].dst.f_h = displayConfig.height;
    win_config[DECON_WIN_UPDATE_IDX].plane_alpha = (int)(1000000000 / displayConfig.vsyncPeriod);
    win_data.fps = (int)(1000000000 / displayConfig.vsyncPeriod);

    HDEBUGLOGD(eDebugDisplayConfig, "(win_config %d) : %dx%d, fps:%d", config,
               win_config[DECON_WIN_UPDATE_IDX].dst.f_w,
               win_config[DECON_WIN_UPDATE_IDX].dst.f_h, win_data.fps);

    ret = ioctl(mDisplayFd, S3CFB_WIN_CONFIG, &win_data);

    if (ret < 0) {
        ALOGE("%s S3CFB_WIN_CONFIG failed errno : %d, ret: %d", __func__, errno, ret);
    }
#elif defined(USES_SET_DISPLAY_MODE_IOCTL)
    struct decon_display_mode display_mode;
    memset(&display_mode, 0, sizeof(decon_display_mode));
    display_mode.index = config;
    display_mode.width = displayConfig.width;
    display_mode.height = displayConfig.height;
    display_mode.fps = (int)(1000000000 / displayConfig.vsyncPeriod);
    display_mode.group = displayConfig.groupId;

    HDEBUGLOGD(eDebugDisplayConfig, "(display_mode %d) : %dx%d, fps:%d groupId: %d", config,
               display_mode.width, display_mode.height, display_mode.fps, display_mode.group);

    ret = ioctl(mDisplayFd, EXYNOS_SET_DISPLAY_MODE, &display_mode);

    if (ret < 0) {
        ALOGE("%s EXYNOS_SET_DISPLAY_MODE failed errno : %d, ret: %d", __func__, errno, ret);
    }
#endif

    /* Those variables should be updated before updateDSCBlockSize() call */
    bool sizeChanged = mXres != displayConfig.width || mYres != displayConfig.height;
    mXres = displayConfig.width;
    mYres = displayConfig.height;

    /* Update DSC block size for window update if resolution is changed */
    if (sizeChanged)
        updateDSCBlockSize();
#endif /* USE_DPU_SET_CONFIG || USES_SET_DISPLAY_MODE_IOCTL */

    return ret;
}

int32_t ExynosDisplayFbInterface::setActiveConfigWithConstraints(hwc2_config_t desiredConfig,
                                                                 displayConfigs_t &displayConfig, bool test) {
    if (test) {
        HDEBUGLOGD(eDebugDisplayConfig, "Check only possbility");
        return NO_ERROR;
    }
    return setActiveConfig(desiredConfig, displayConfig);
}

int32_t ExynosDisplayFbInterface::getDisplayVsyncPeriod(
    hwc2_vsync_period_t *outVsyncPeriod) {
    int32_t ret = HWC2_ERROR_NONE;

    uint64_t timestamp = 0;
    uint32_t fps = 60;

    ret = getVsyncAndFps(&timestamp, &fps);

    if ((ret != HWC2_ERROR_NONE) && (ret != HWC2_ERROR_UNSUPPORTED)) {
        ALOGI("Vsync period read failed");
    } else {
        if (fps == 0 || fps > 1000000000) {
            ALOGW("%s: fps value(%u)is invalid, so set default fps(60)", __func__, fps);
            fps = 60;
        }
        *outVsyncPeriod = (uint32_t)(1000000000 / fps);
        HDEBUGLOGD(eDebugDisplayConfig, "%s, %d", __func__, *outVsyncPeriod);
    }

    return ret;
}

int32_t ExynosDisplayFbInterface::getVsyncAndFps(uint64_t *timestamp, uint32_t *fps) {
    int err = 0;
    char _timestamp[256] = {
        0,
    };
    char *_fps;

    err = lseek(mVsyncFd, 0, SEEK_SET);
    if (err < 0) {
        ALOGE("Can't fine vsync node : %s", strerror(errno));
        return -1;
    }

    err = read(mVsyncFd, _timestamp, sizeof(_timestamp) - 1);
    if (err < 0) {
        ALOGE("Can't read vsync node : %s", strerror(errno));
        return -1;
    }

    if (strchr(_timestamp, '\n') == NULL)
        return -1;

    *timestamp = strtoull(_timestamp, NULL, 0);

    _fps = strchr(_timestamp, ' ');

    if ((_fps != NULL) && (strchr(_fps, '\n') != NULL))
        *fps = atoi(_fps);
    else
        return HWC2_ERROR_UNSUPPORTED;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::getDisplayVsyncTimestamp(uint64_t *outVsyncTimestamp) {
    int32_t ret = HWC2_ERROR_NONE;

    uint64_t timestamp = 0;
    uint32_t fps = 60;

    ret = getVsyncAndFps(&timestamp, &fps);

    if (ret == HWC2_ERROR_UNSUPPORTED)
        ret = HWC2_ERROR_NONE;

    if (ret != HWC2_ERROR_NONE)
        ALOGI("Vsync timestamp read failed");
    else
        *outVsyncTimestamp = timestamp;

    HDEBUGLOGD(eDebugDisplayConfig, "%s, %llu", __func__, (unsigned long long)*outVsyncTimestamp);

    return ret;
}

void ExynosDisplayFbInterface::dumpDisplayConfigs() {
}

int32_t ExynosDisplayFbInterface::getColorModes(
    uint32_t *outNumModes,
    int32_t *outModes) {
    uint32_t colorModeNum = 0;
    int32_t ret = 0;
    if ((ret = ioctl(mDisplayFd, EXYNOS_GET_COLOR_MODE_NUM, &colorModeNum)) < 0) {
        *outNumModes = 1;

        ALOGI("%s:: is not supported", __func__);
        if (outModes != NULL) {
            outModes[0] = HAL_COLOR_MODE_NATIVE;
        }
        return HWC2_ERROR_NONE;
    }

    if (outModes == NULL) {
        ALOGI("%s:: Supported color modes (%d)", __func__, colorModeNum);
        *outNumModes = colorModeNum;
        return HWC2_ERROR_NONE;
    }

    if (*outNumModes != colorModeNum) {
        ALOGE("%s:: invalid outNumModes(%d), should be(%d)", __func__, *outNumModes, colorModeNum);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    for (uint32_t i = 0; i < colorModeNum; i++) {
        struct decon_color_mode_info colorMode;
        colorMode.index = i;
        if ((ret = ioctl(mDisplayFd, EXYNOS_GET_COLOR_MODE, &colorMode)) < 0) {
            return HWC2_ERROR_UNSUPPORTED;
        }
        ALOGI("\t[%d] mode %d", i, colorMode.color_mode);
        outModes[i] = colorMode.color_mode;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::setColorMode(int32_t mode, int32_t dqe_fd) {
    int32_t ret = NO_ERROR;

    if ((ret = ioctl(mDisplayFd, EXYNOS_SET_COLOR_MODE, &mode)) < 0) {
        HWC_LOGE(mDisplayIdentifier, "set color mode ioctl failed errno : %d, ret: %d",
                 errno, ret);
        return HWC2_ERROR_UNSUPPORTED;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::setCursorPositionAsync(uint32_t x_pos, uint32_t y_pos) {
    struct decon_user_window win_pos;
    win_pos.x = x_pos;
    win_pos.y = y_pos;
    ioctl(mDisplayFd, S3CFB_WIN_POSITION, &win_pos);
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::updateHdrCapabilities(std::vector<int32_t> &outTypes,
                                                        float *outMaxLuminance, float *outMaxAverageLuminance, float *outMinLuminance) {
    struct decon_hdr_capabilities_info outInfo = {};
    if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES_NUM, &outInfo) < 0) {
        ALOGE("updateHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES_NUM ioctl failed");
        return -1;
    }

    // Save to member variables
    *outMaxLuminance = static_cast<float>(outInfo.max_luminance) / DISPLAY_LUMINANCE_UNIT;
    *outMaxAverageLuminance = static_cast<float>(outInfo.max_average_luminance) / DISPLAY_LUMINANCE_UNIT;
    *outMinLuminance = static_cast<float>(outInfo.min_luminance) / DISPLAY_LUMINANCE_UNIT;
    ALOGI("hdrTypeNum(%d), maxLuminance(%f), maxAverageLuminance(%f), minLuminance(%f)",
          outInfo.out_num, *outMaxLuminance, *outMaxAverageLuminance, *outMinLuminance);

    struct decon_hdr_capabilities outData = {};

    if (ioctl(mDisplayFd, S3CFB_GET_HDR_CAPABILITIES, &outData) < 0) {
        ALOGE("updateHdrCapabilities: S3CFB_GET_HDR_CAPABILITIES ioctl Failed");
        return -1;
    }
    outTypes.clear();
    for (uint32_t i = 0; i < static_cast<uint32_t>(outInfo.out_num); i++) {
        // Save to member variables
        outTypes.push_back(outData.out_types[i]);
        HDEBUGLOGD(eDebugHWC, "HWC2: Types : %d", outTypes[i]);
    }
    return 0;
}

decon_idma_type ExynosDisplayFbInterface::getDeconDMAType(
    uint32_t __unused type, uint32_t __unused index) {
    return MAX_DECON_DMA_TYPE;
}

int32_t ExynosDisplayFbInterface::configFromDisplayConfig(decon_win_config &config,
                                                          const exynos_win_config_data &display_config) {
    if (display_config.state == display_config.WIN_STATE_DISABLED)
        return NO_ERROR;

    config.dst = display_config.dst;
    config.plane_alpha = 255;

    int32_t planeAlpha = (int)((255 * display_config.plane_alpha) + 0.5);
    if ((planeAlpha >= 0) && (planeAlpha < 255)) {
        config.plane_alpha = planeAlpha;
    }
    if ((config.blending = halBlendingToDpuBlending(display_config.blending)) >= DECON_BLENDING_MAX) {
        HWC_LOGE(mDisplayIdentifier, "%s:: config has invalid blending(0x%8x)",
                 __func__, display_config.blending);
        return -EINVAL;
    }

    ExynosMPP *assignedMPP = display_config.assignedMPP;
    if (assignedMPP == nullptr) {
        HWC_LOGE(mDisplayIdentifier, "%s:: config has invalid idma_type, assignedMPP is NULL",
                 __func__);
        return -EINVAL;
    } else if ((config.idma_type = getDeconDMAType(
                    assignedMPP->mPhysicalType,
                    assignedMPP->mPhysicalIndex)) == MAX_DECON_DMA_TYPE) {
        HWC_LOGE(mDisplayIdentifier, "%s:: config has invalid idma_type, assignedMPP(%s)",
                 __func__, assignedMPP->mName.c_str());
        return -EINVAL;
    }

    if (display_config.state == display_config.WIN_STATE_COLOR) {
        config.state = config.DECON_WIN_STATE_COLOR;
        config.color = display_config.color;
        if (!((planeAlpha >= 0) && (planeAlpha <= 255)))
            config.plane_alpha = 0;
    } else if ((display_config.state == display_config.WIN_STATE_BUFFER) ||
               (display_config.state == display_config.WIN_STATE_CURSOR)) {
        if (display_config.state == display_config.WIN_STATE_BUFFER)
            config.state = config.DECON_WIN_STATE_BUFFER;
        else
            config.state = config.DECON_WIN_STATE_CURSOR;

        config.fd_idma[0] = display_config.fd_idma[0];
        config.fd_idma[1] = display_config.fd_idma[1];
        config.fd_idma[2] = display_config.fd_idma[2];
        config.acq_fence = display_config.acq_fence;
        config.rel_fence = display_config.rel_fence;
        if ((config.format = display_config.format.dpuFormat()) == DECON_PIXEL_FORMAT_MAX) {
            HWC_LOGE(mDisplayIdentifier, "%s:: config has invalid format(%s)",
                     __func__, display_config.format.name().c_str());
            return -EINVAL;
        }
        config.dpp_parm.comp_src = display_config.comp_src;
        config.dpp_parm.rot = (dpp_rotate)halTransformToDpuRot(display_config.transform);
        config.dpp_parm.eq_mode = halDataSpaceToDisplayParam(display_config);
        if (display_config.hdr_enable)
            config.dpp_parm.hdr_std = halTransferToDisplayParam(display_config);
        config.dpp_parm.min_luminance = display_config.min_luminance;
        config.dpp_parm.max_luminance = display_config.max_luminance;
        config.block_area = display_config.block_area;
        config.transparent_area = display_config.transparent_area;
        config.opaque_area = display_config.opaque_area;
        config.src = display_config.src;
        config.protection = display_config.protection;
        if (display_config.compressionInfo.type == COMP_TYPE_AFBC)
            config.compression = 1;
        else
            config.compression = 0;

        setFdLut(&config, display_config.fd_lut);
    }
    return NO_ERROR;
}

decon_idma_type ExynosDisplayFbInterface::getSubDeconDMAType(decon_idma_type channel) {
    /* Implement this to module */
    return static_cast<decon_idma_type>(DECON_WRITEBACK_IDX + 1);
}

int32_t ExynosDisplayFbInterface::preProcessForVirtual8K(struct decon_win_config *__unused savedVirtualWinConfig) {
    /* Implement this to module */
    return -1;
}

int32_t ExynosDisplayFbInterface::postProcessForVirtual8K(struct decon_win_config __unused savedVirtualWinConfig) {
    /* Implement this to module */
    return NO_ERROR;
}

int32_t ExynosDisplayFbInterface::deliverWinConfigData(exynos_dpu_data &dpuData) {
    android::String8 result;
    clearFbWinConfigData(mFbConfigData);
    struct decon_win_config *config = mFbConfigData.config;
    for (uint32_t i = 0; i < NUM_HW_WINDOWS; i++) {
        int32_t ret = configFromDisplayConfig(mFbConfigData.config[i],
                                              dpuData.configs[i]);
        if (ret != NO_ERROR) {
            HWC_LOGE(mDisplayIdentifier, "configFromDisplayConfig config[%d] fail", i);
            return ret;
        }
    }
#ifdef USE_DQE_INTERFACE
    mFbConfigData.fd_dqe = dpuData.fd_dqe;
#endif
    if (dpuData.enable_win_update && mDSCHSliceNum && mDSCYSliceSize) {
        hwc_rect merge_rect = {dpuData.win_update_region.x,
                               dpuData.win_update_region.y,
                               static_cast<int>(dpuData.win_update_region.x + dpuData.win_update_region.w),
                               static_cast<int>(dpuData.win_update_region.y + dpuData.win_update_region.h)};
        alignDSCBlockSize(merge_rect);

        size_t winUpdateInfoIdx = DECON_WIN_UPDATE_IDX;
        config[winUpdateInfoIdx].state = config[winUpdateInfoIdx].DECON_WIN_STATE_UPDATE;
        config[winUpdateInfoIdx].dst.x = merge_rect.left;
        config[winUpdateInfoIdx].dst.w = merge_rect.right - merge_rect.left;
        config[winUpdateInfoIdx].dst.y = merge_rect.top;
        config[winUpdateInfoIdx].dst.h = merge_rect.bottom - merge_rect.top;
    }

    if (dpuData.enable_readback)
        setReadbackConfig(dpuData, config);

    if (dpuData.enable_standalone_writeback)
        setStandAloneWritebackConfig(dpuData, config);

    struct decon_win_config savedVirtualWinConfig;
    mVirtual8KDPPIndex = preProcessForVirtual8K(&savedVirtualWinConfig);

    dumpFbWinConfigInfo(result, mFbConfigData, true);

    int32_t ret = 0;
    {
        ATRACE_CALL();
        ret = ioctl(mDisplayFd, S3CFB_WIN_CONFIG, &mFbConfigData);
    }

    if (mVirtual8KDPPIndex >= 0) {
        postProcessForVirtual8K(savedVirtualWinConfig);
    }

    if (ret) {
        result.clear();
        result.appendFormat("WIN_CONFIG ioctl error\n");
        HWC_LOGE(mDisplayIdentifier, "%s", dumpFbWinConfigInfo(result, mFbConfigData).c_str());
        return ret;
    } else {
        dpuData.present_fence = mFbConfigData.present_fence;
        struct decon_win_config *config = mFbConfigData.config;
        for (uint32_t i = 0; i < NUM_HW_WINDOWS; i++) {
            dpuData.configs[i].rel_fence = config[i].rel_fence;
        }
        if (dpuData.enable_readback && (DECON_READBACK_IDX >= 0)) {
            int readbackAcqFence = config[DECON_READBACK_IDX].acq_fence;
            if (readbackAcqFence >= 0) {
                if (dpuData.readback_info.acq_fence >= 0) {
                    dpuData.readback_info.acq_fence =
                        mFenceTracer.fence_close(dpuData.readback_info.acq_fence,
                                                 mDisplayIdentifier, FENCE_TYPE_READBACK_ACQUIRE, FENCE_IP_DPP,
                                                 "display::setReadbackBufferAcqFence: acq_fence");
                    HWC_LOGE(mDisplayIdentifier, "previous readback out fence is not delivered to framework");
                }
                dpuData.readback_info.acq_fence = readbackAcqFence;
            }
        }
        if (dpuData.enable_standalone_writeback && (DECON_WRITEBACK_IDX >= 0)) {
            // virtual display will use only present fence
            if (config[DECON_WRITEBACK_IDX].acq_fence >= 0) {
                config[DECON_WRITEBACK_IDX].acq_fence =
                    mFenceTracer.fence_close(config[DECON_WRITEBACK_IDX].acq_fence,
                                             mDisplayIdentifier, FENCE_TYPE_DST_ACQUIRE, FENCE_IP_OUTBUF,
                                             "FbInterface::deliverWinConfigData: standalone_writeback.acq_fence");
            }
        }
    }

    return NO_ERROR;
}

int32_t ExynosDisplayFbInterface::clearDisplay() {
    if (mDisplayFd < 0)
        return 0;

    struct decon_win_config_data win_data;
    memset(&win_data, 0, sizeof(win_data));
    win_data.present_fence = -1;
    struct decon_win_config *config = win_data.config;
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        config[i].acq_fence = -1;
        config[i].rel_fence = -1;
        setFdLut(&config[i] , -1);
    }

#ifdef USE_DQE_INTERFACE
    win_data.fd_dqe = -1;
#endif

    uint32_t default_window = 0;
    default_window = mDisplayInfo.baseWindowIndex;

#if defined(HWC_CLEARDISPLAY_WITH_COLORMAP)
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        if (i == default_window) {
            config[i].state = config[i].DECON_WIN_STATE_COLOR;
            config[i].idma_type = mDisplayInfo.defaultDMA;
            config[i].color = 0x0;
            config[i].dst.x = 0;
            config[i].dst.y = 0;
            config[i].dst.w = mDisplayInfo.xres;
            config[i].dst.h = mDisplayInfo.yres;
            config[i].dst.f_w = mDisplayInfo.xres;
            config[i].dst.f_h = mDisplayInfo.yres;
        } else
            config[i].state = config[i].DECON_WIN_STATE_DISABLED;
    }
#endif

    win_data.present_fence = -1;

    const int ret = ioctl(mDisplayFd, S3CFB_WIN_CONFIG, &win_data);
    if (ret < 0)
        HWC_LOGE(mDisplayIdentifier, "ioctl S3CFB_WIN_CONFIG failed to clear screen: %s",
                 strerror(errno));

    if (win_data.present_fence > 0)
        mFenceTracer.fence_close(win_data.present_fence,
                                 mDisplayIdentifier, FENCE_TYPE_PRESENT, FENCE_IP_DPP,
                                 "FbInterface::clearDisplay: present_fence");
    return ret;
}

int32_t ExynosDisplayFbInterface::disableSelfRefresh(uint32_t disable) {
    return ioctl(mDisplayFd, S3CFB_DECON_SELF_REFRESH, &disable);
}

int32_t ExynosDisplayFbInterface::setForcePanic() {
    if (exynosHWCControl.forcePanic == 0)
        return NO_ERROR;

    usleep(20000);
    return ioctl(mDisplayFd, S3CFB_FORCE_PANIC, 0);
}

void ExynosDisplayFbInterface::clearFbWinConfigData(decon_win_config_data &winConfigData) {
    memset(&winConfigData, 0, sizeof(winConfigData));
    winConfigData.fd_odma = -1;
    winConfigData.present_fence = -1;
    struct decon_win_config *config = winConfigData.config;
    /* init */
    for (size_t i = 0; i < NUM_HW_WINDOWS; i++) {
        config[i].fd_idma[0] = -1;
        config[i].fd_idma[1] = -1;
        config[i].fd_idma[2] = -1;
        config[i].acq_fence = -1;
        config[i].rel_fence = -1;
        setFdLut(&config[i], -1);
    }
    if (DECON_WIN_UPDATE_IDX >= 0) {
        config[DECON_WIN_UPDATE_IDX].fd_idma[0] = -1;
        config[DECON_WIN_UPDATE_IDX].fd_idma[1] = -1;
        config[DECON_WIN_UPDATE_IDX].fd_idma[2] = -1;
        config[DECON_WIN_UPDATE_IDX].acq_fence = -1;
        config[DECON_WIN_UPDATE_IDX].rel_fence = -1;
    }
    if (DECON_READBACK_IDX >= 0) {
        config[DECON_READBACK_IDX].fd_idma[0] = -1;
        config[DECON_READBACK_IDX].fd_idma[1] = -1;
        config[DECON_READBACK_IDX].fd_idma[2] = -1;
        config[DECON_READBACK_IDX].acq_fence = -1;
        config[DECON_READBACK_IDX].rel_fence = -1;
    }
    if (DECON_WRITEBACK_IDX >= 0) {
        config[DECON_WRITEBACK_IDX].fd_idma[0] = -1;
        config[DECON_WRITEBACK_IDX].fd_idma[1] = -1;
        config[DECON_WRITEBACK_IDX].fd_idma[2] = -1;
        config[DECON_WRITEBACK_IDX].acq_fence = -1;
        config[DECON_WRITEBACK_IDX].rel_fence = -1;
    }
#ifdef USE_DQE_INTERFACE
    winConfigData.fd_dqe = -1;
#endif
}

dpp_csc_eq ExynosDisplayFbInterface::halDataSpaceToDisplayParam(const exynos_win_config_data &config) {
    uint32_t cscEQ = 0;
    android_dataspace dataspace = dataspaceFromConfig(config);
    ExynosMPP *otfMPP = config.assignedMPP;
    uint32_t standard = (dataspace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t range = (dataspace & HAL_DATASPACE_RANGE_MASK);

    if (otfMPP == NULL) {
        HWC_LOGE(mDisplayIdentifier, "%s:: assignedMPP is NULL", __func__);
        return (dpp_csc_eq)cscEQ;
    }

    if (dataspace_standard_map.find(standard) != dataspace_standard_map.end())
        cscEQ = dataspace_standard_map.at(standard).eq_mode;
    else
        cscEQ = CSC_UNSPECIFIED;

    if ((otfMPP->mAttr & MPP_ATTR_WCG) == 0) {
        switch (cscEQ) {
        case CSC_BT_709:
        case CSC_BT_601:
        case CSC_BT_2020:
        case CSC_DCI_P3:
            break;
        default:
            cscEQ = CSC_UNSPECIFIED;
            break;
        }
        switch (range) {
        case HAL_DATASPACE_RANGE_FULL:
        case HAL_DATASPACE_RANGE_LIMITED:
            break;
        default:
            range = HAL_DATASPACE_RANGE_UNSPECIFIED;
            break;
        }
    }

    if (dataspace_range_map.find(range) != dataspace_range_map.end())
        cscEQ |= dataspace_range_map.at(range);
    else
        cscEQ |= (CSC_RANGE_UNSPECIFIED << CSC_RANGE_SHIFT);

    return (dpp_csc_eq)cscEQ;
}

dpp_hdr_standard ExynosDisplayFbInterface::halTransferToDisplayParam(const exynos_win_config_data &config) {
    android_dataspace dataspace = dataspaceFromConfig(config);
    ExynosMPP *otfMPP = config.assignedMPP;

    uint32_t transfer = (dataspace & HAL_DATASPACE_TRANSFER_MASK);
    dpp_hdr_standard ret = DPP_HDR_OFF;

    if (otfMPP == NULL)
        return ret;

    if ((otfMPP->mAttr & MPP_ATTR_WCG) == 0) {
        if (hasHdrInfo(dataspace) == false)
            return DPP_HDR_OFF;
    }

    if (((otfMPP->mAttr & MPP_ATTR_HDR10) == 0) &&
        ((otfMPP->mAttr & MPP_ATTR_WCG) == 0) &&
        ((otfMPP->mAttr & MPP_ATTR_HDR10PLUS) == 0))
        return DPP_HDR_OFF;

    if (dataspace_transfer_map.find(transfer) != dataspace_transfer_map.end())
        ret = dataspace_transfer_map.at(transfer).hdr_std;

    return ret;
}

android_dataspace ExynosDisplayFbInterface::dpuDataspaceToHalDataspace(uint32_t dpu_dataspace) {
    uint32_t hal_dataspace = HAL_DATASPACE_UNKNOWN;

    uint32_t display_standard =
        (dpu_dataspace & DPU_DATASPACE_STANDARD_MASK);
    uint32_t display_range =
        (dpu_dataspace & DPU_DATASPACE_RANGE_MASK);
    uint32_t display_transfer =
        ((dpu_dataspace & DPU_DATASPACE_TRANSFER_MASK) >> DPU_DATASPACE_TRANSFER_SHIFT);

    for (auto it = dataspace_standard_map.begin(); it != dataspace_standard_map.end(); it++) {
        if (it->second.eq_mode == display_standard) {
            hal_dataspace |= it->first;
            break;
        }
    }

    for (auto it = dataspace_range_map.begin(); it != dataspace_range_map.end(); it++) {
        if (it->second == display_range) {
            hal_dataspace |= it->first;
            break;
        }
    }

    for (auto it = dataspace_transfer_map.begin(); it != dataspace_transfer_map.end(); it++) {
        if (it->second.hdr_std == display_transfer) {
            hal_dataspace |= it->first;
            break;
        }
    }
    return (android_dataspace)hal_dataspace;
}

String8 &ExynosDisplayFbInterface::dumpFbWinConfigInfo(String8 &result,
                                                       decon_win_config_data &fbConfig, bool debugPrint) {
    /* print log only if eDebugDisplayInterfaceConfig flag is set when debugPrint is true */
    if (debugPrint &&
        (hwcCheckDebugMessages(eDebugDisplayInterfaceConfig) == false))
        return result;

    result.appendFormat("present_fence(%d)\n", mFbConfigData.present_fence);
    struct decon_win_config *config = mFbConfigData.config;
    for (uint32_t i = 0; i <= NUM_HW_WINDOWS; i++) {
        decon_win_config &c = config[i];
        String8 configString;
        configString.appendFormat("win[%d] state = %u\n", i, c.state);
        if (c.state == c.DECON_WIN_STATE_COLOR) {
            configString.appendFormat("\t\tx = %d, y = %d, width = %d, height = %d, color = %u, alpha = %u\n",
                                      c.dst.x, c.dst.y, c.dst.w, c.dst.h, c.color, c.plane_alpha);
        } else /* if (c.state != c.DECON_WIN_STATE_DISABLED) */ {
            configString.appendFormat("\t\tidma = %d, fd = (%d, %d, %d), acq_fence = %d, rel_fence = %d "
                                      "src_f_w = %u, src_f_h = %u, src_x = %d, src_y = %d, src_w = %u, src_h = %u, "
                                      "dst_f_w = %u, dst_f_h = %u, dst_x = %d, dst_y = %d, dst_w = %u, dst_h = %u, "
                                      "format = %u, pa = %d, rot = %d, eq_mode = 0x%8x, hdr_std = %d, blending = %u, "
                                      "protection = %u, compression = %d, compression_src = %d, transparent(x:%d, y:%d, w:%d, h:%d), "
                                      "block(x:%d, y:%d, w:%d, h:%d), min/max_luminance(%d, %d)\n",
                                      c.idma_type, c.fd_idma[0], c.fd_idma[1], c.fd_idma[2],
                                      c.acq_fence, c.rel_fence,
                                      c.src.f_w, c.src.f_h, c.src.x, c.src.y, c.src.w, c.src.h,
                                      c.dst.f_w, c.dst.f_h, c.dst.x, c.dst.y, c.dst.w, c.dst.h,
                                      c.format, c.plane_alpha, c.dpp_parm.rot, c.dpp_parm.eq_mode,
                                      c.dpp_parm.hdr_std, c.blending, c.protection,
                                      c.compression, c.dpp_parm.comp_src,
                                      c.transparent_area.x, c.transparent_area.y, c.transparent_area.w, c.transparent_area.h,
                                      c.opaque_area.x, c.opaque_area.y, c.opaque_area.w, c.opaque_area.h,
                                      c.dpp_parm.min_luminance, c.dpp_parm.max_luminance);
            configString.appendFormat("fd_lut = %d\n", getFdLut(&c));
        }

        if (debugPrint)
            ALOGD("%s", configString.c_str());
        else
            result.append(configString);
    }
    return result;
}

uint32_t ExynosDisplayFbInterface::getMaxWindowNum() {
    return NUM_HW_WINDOWS;
}

int32_t ExynosDisplayFbInterface::setColorTransform(const float *matrix, int32_t hint, int32_t dqe_fd) {
    struct decon_color_transform_info transform_info;
    transform_info.hint = hint;
    for (uint32_t i = 0; i < DECON_MATRIX_ELEMENT_NUM; i++) {
        transform_info.matrix[i] = (int)round(matrix[i] * 65536);
    }
    int ret = ioctl(mDisplayFd, EXYNOS_SET_COLOR_TRANSFORM, &transform_info);
    if (ret < 0) {
        ALOGE("%s:: is not supported", __func__);
        // Send initialization matrix
        uint32_t row = (uint32_t)sqrt(DECON_MATRIX_ELEMENT_NUM);
        for (uint32_t i = 0; i < DECON_MATRIX_ELEMENT_NUM; i++) {
            if (i % (row + 1) == 0)
                transform_info.matrix[i] = 65536;
            else
                transform_info.matrix[i] = 0;
        }
        int reset_ret = ioctl(mDisplayFd, EXYNOS_SET_COLOR_TRANSFORM, &transform_info);
        if (reset_ret < 0)
            ALOGE("%s:: init ioctl is not operated", __func__);
        return HWC2_ERROR_UNSUPPORTED;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::getRenderIntents(int32_t mode, uint32_t *outNumIntents,
                                                   int32_t *outIntents) {
    struct decon_render_intents_num_info intents_num_info;
    intents_num_info.color_mode = mode;
    int32_t ret = 0;
    if ((ret = ioctl(mDisplayFd, EXYNOS_GET_RENDER_INTENTS_NUM, &intents_num_info)) < 0) {
        *outNumIntents = 1;

        ALOGI("%s:: is not supported", __func__);
        if (outIntents != NULL) {
            outIntents[0] = HAL_RENDER_INTENT_COLORIMETRIC;
        }
        return HWC2_ERROR_NONE;
    }

    if (outIntents == NULL) {
        ALOGI("%s:: Supported intent (mode: %d, num: %d)",
              __func__, mode, intents_num_info.render_intent_num);
        *outNumIntents = intents_num_info.render_intent_num;
        return HWC2_ERROR_NONE;
    }

    if (*outNumIntents != intents_num_info.render_intent_num) {
        ALOGE("%s:: invalid outIntents(mode: %d, num: %d), should be(%d)",
              __func__, mode, *outIntents, intents_num_info.render_intent_num);
        return HWC2_ERROR_BAD_PARAMETER;
    }

    for (uint32_t i = 0; i < intents_num_info.render_intent_num; i++) {
        struct decon_render_intent_info render_intent;
        render_intent.color_mode = mode;
        render_intent.index = i;
        if ((ret = ioctl(mDisplayFd, EXYNOS_GET_RENDER_INTENT, &render_intent)) < 0) {
            return HWC2_ERROR_UNSUPPORTED;
        }
        ALOGI("\t[%d] intent %d", i, render_intent.render_intent);
        outIntents[i] = render_intent.render_intent;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::setColorModeWithRenderIntent(int32_t mode,
                                                               int32_t intent, int32_t dqe_fd) {
    struct decon_color_mode_with_render_intent_info color_info;
    color_info.color_mode = mode;
    color_info.render_intent = intent;

    if (ioctl(mDisplayFd, EXYNOS_SET_COLOR_MODE_WITH_RENDER_INTENT, &color_info) < 0) {
        ALOGI("%s:: is not supported", __func__);
        return HWC2_ERROR_UNSUPPORTED;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayFbInterface::getReadbackBufferAttributes(
    int32_t * /*android_pixel_format_t*/ outFormat,
    int32_t * /*android_dataspace_t*/ outDataspace) {
    int32_t ret = NO_ERROR;
    struct decon_readback_attribute readback_attribute;
    readback_attribute.format = 0;
    readback_attribute.dataspace = 0;
    if ((ret = ioctl(mDisplayFd, EXYNOS_GET_READBACK_ATTRIBUTE,
                     &readback_attribute)) == NO_ERROR) {
        *outFormat = DpuFormatToHalFormat(readback_attribute.format);
        *outDataspace = dpuDataspaceToHalDataspace(readback_attribute.dataspace);
    } else {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    return HWC2_ERROR_NONE;
}

void ExynosDisplayFbInterface::setReadbackConfig(
    exynos_dpu_data &dpuData, decon_win_config *config) {
    if (dpuData.readback_info.handle == NULL) {
        HWC_LOGE(mDisplayIdentifier, "%s:: readback is enabled but readback buffer is NULL", __func__);
    } else if (DECON_READBACK_IDX < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: readback is enabled but window index for readback is not defined", __func__);
        if (dpuData.readback_info.rel_fence >= 0) {
            dpuData.readback_info.rel_fence =
                mFenceTracer.fence_close(dpuData.readback_info.rel_fence,
                                         mDisplayIdentifier, FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB,
                                         "setReadbackConfig:: readback.rel_fence");
        }
        dpuData.enable_readback = false;
    } else {
        /* set winconfig data for readback buffer */
        exynos_writeback_info &readback_info = dpuData.readback_info;
        ExynosGraphicBufferMeta gmeta(dpuData.readback_info.handle);

        config[DECON_READBACK_IDX].idma_type = IDMA(ODMA_WB);
        config[DECON_READBACK_IDX].state = config[DECON_READBACK_IDX].DECON_WIN_STATE_BUFFER;
        config[DECON_READBACK_IDX].fd_idma[0] = gmeta.fd;
        config[DECON_READBACK_IDX].fd_idma[1] = gmeta.fd1;
        config[DECON_READBACK_IDX].fd_idma[2] = gmeta.fd2;
        config[DECON_READBACK_IDX].format = halFormatToDpuFormat(gmeta.format);
        uint32_t xres = mDisplayInfo.xres;
        uint32_t yres = mDisplayInfo.yres;
        config[DECON_READBACK_IDX].src = {0, 0, xres, yres, gmeta.stride, gmeta.vstride};
        config[DECON_READBACK_IDX].dst = {0, 0, xres, yres, gmeta.stride, gmeta.vstride};

        /*
         * This will be closed by ExynosDisplay::setReleaseFences()
         * after HWC delivers fence because display driver doesn't close it
         */
        config[DECON_READBACK_IDX].rel_fence = readback_info.rel_fence;

        ALOGD("readback_config(win[%u]) state = %u \n\t"
              "fd=(%d, %d, %d), rel_fence=%d, dst_f_w=%u, dst_f_h=%u,"
              "dst_x=%d, dst_y=%d, dst_w=%u, dst_h=%u, format=%u",
              DECON_READBACK_IDX,
              config[DECON_READBACK_IDX].state,
              config[DECON_READBACK_IDX].fd_idma[0],
              config[DECON_READBACK_IDX].fd_idma[1],
              config[DECON_READBACK_IDX].fd_idma[2],
              config[DECON_READBACK_IDX].rel_fence,
              config[DECON_READBACK_IDX].dst.f_w, config[DECON_READBACK_IDX].dst.f_h,
              config[DECON_READBACK_IDX].dst.x, config[DECON_READBACK_IDX].dst.y,
              config[DECON_READBACK_IDX].dst.w, config[DECON_READBACK_IDX].dst.h,
              config[DECON_READBACK_IDX].format);
    }
}

void ExynosDisplayFbInterface::setRepeaterBuffer(bool val) {
    ALOGI("%s:: val(%d)", __func__, val);
    ioctl(mDisplayFd, EXYNOS_SET_REPEATER_BUF, &val);
}

void ExynosDisplayFbInterface::setStandAloneWritebackConfig(
    exynos_dpu_data &dpuData, decon_win_config *config) {
    int wfd_mode = mDisplayInfo.isWFDState;

    if (dpuData.standalone_writeback_info.handle == NULL) {
        HWC_LOGE(mDisplayIdentifier, "%s:: standalone writeback buffer is NULL", __func__);
    } else if (DECON_WRITEBACK_IDX < 0) {
        HWC_LOGE(mDisplayIdentifier, "%s:: window index for standalone writeback is not defined", __func__);
        if (dpuData.standalone_writeback_info.rel_fence >= 0) {
            dpuData.standalone_writeback_info.rel_fence =
                mFenceTracer.fence_close(dpuData.standalone_writeback_info.rel_fence,
                                         mDisplayIdentifier, FENCE_TYPE_DST_RELEASE, FENCE_IP_OUTBUF,
                                         "setStandAloneWritebackConfig:: standalone_writeback.rel_fence");
        }
        dpuData.enable_standalone_writeback = false;
    } else {
        decon_win_config &cfgWB = config[DECON_WRITEBACK_IDX];
        ExynosGraphicBufferMeta gmeta(dpuData.standalone_writeback_info.handle);

        cfgWB.state = cfgWB.DECON_WIN_STATE_BUFFER;
        if (wfd_mode == LLWFD) {
            cfgWB.fd_idma[0] = -1;
            cfgWB.fd_idma[1] = -1;
            cfgWB.fd_idma[2] = -1;
            cfgWB.src.f_w = cfgWB.dst.f_w = mDisplayInfo.xres;
            cfgWB.src.f_h = cfgWB.dst.f_h = mDisplayInfo.yres;
            cfgWB.format = halFormatToDpuFormat(HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN);
        } else {
            cfgWB.fd_idma[0] = gmeta.fd;
            cfgWB.fd_idma[1] = gmeta.fd1;
            cfgWB.fd_idma[2] = gmeta.fd2;
            cfgWB.src.f_w = cfgWB.dst.f_w = gmeta.stride;
            cfgWB.src.f_h = cfgWB.dst.f_h = gmeta.vstride;
            cfgWB.format = halFormatToDpuFormat(gmeta.format);
        }

        cfgWB.idma_type = ODMA_WB;
        cfgWB.src.x = cfgWB.dst.x = 0;
        cfgWB.src.y = cfgWB.dst.y = 0;
        cfgWB.src.w = cfgWB.dst.w = mDisplayInfo.xres;
        cfgWB.src.h = cfgWB.dst.h = mDisplayInfo.yres;
        cfgWB.rel_fence = dpuData.standalone_writeback_info.rel_fence;
        cfgWB.protection = getDrmMode(gmeta.producer_usage) == SECURE_DRM ? 1 : 0;
        HDEBUGLOGD(eDebugVirtualDisplay, "%s:: fd(%d, %d, %d), format(0x%x), w(%d), h(%d), f_w(%d), f_h(%d), \
                   rel_fence(%d), protection(%d), wfd_mode(%d)",
                   __func__, cfgWB.fd_idma[0], cfgWB.fd_idma[1], cfgWB.fd_idma[2],
                   gmeta.format, mDisplayInfo.xres, mDisplayInfo.yres,
                   gmeta.stride, gmeta.vstride, cfgWB.rel_fence, cfgWB.protection, wfd_mode);
    }
}

uint8_t EDID_SAMPLE[] = {
    0x00 ,0xFF ,0xFF ,0xFF ,0xFF ,0xFF ,0xFF ,0x00 ,0x4C ,0x2D ,0x20 ,0x20 ,0x24 ,0x42 ,0x34 ,0x01,
    0xFF ,0x12 ,0x01 ,0x04 ,0xA4 ,0x00 ,0x02 ,0x64 ,0x1C ,0x60 ,0x41 ,0xA6 ,0x56 ,0x4A ,0x9C ,0x25,
    0x12 ,0x50 ,0x54 ,0x00 ,0x00 ,0x00 ,0x95 ,0x00 ,0x01 ,0x00 ,0x01 ,0x00 ,0x01 ,0x00 ,0x01 ,0x01,
    0x01 ,0x01 ,0x01 ,0x01 ,0x01 ,0x01 ,0x7C ,0x2E ,0xA0 ,0xA0 ,0x50 ,0xE0 ,0x1E ,0xB0 ,0x30 ,0x20,
    0x36 ,0x00 ,0xB1 ,0x0F ,0x11 ,0x00 ,0x00 ,0x1A ,0x00 ,0x00 ,0x00 ,0xFD ,0x00 ,0x38 ,0x4B ,0x1E,
    0x51 ,0x0E ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0x00 ,0xFC ,0x00 ,0x53,
    0x79 ,0x6E ,0x63 ,0x4D ,0x61 ,0x73 ,0x74 ,0x65 ,0x72 ,0x0A ,0x20 ,0x20 ,0x00 ,0x00 ,0x00 ,0xFF,
    0x00 ,0x48 ,0x56 ,0x43 ,0x51 ,0x37 ,0x33 ,0x32 ,0x39 ,0x30 ,0x36 ,0x0A ,0x20 ,0x20 ,0x00 ,0x1D,
};

int32_t ExynosDisplayFbInterface::getDisplayIdentificationData(uint8_t *outPort,
                                                               uint32_t *outDataSize, uint8_t *outData) {
    if (outData == NULL) {
        /* * DisplayId = (manufactureId <<40) | (displayNameHash<<8) | Port
         * - Port = uint8_t      Encoder Type : Bit7 - DisplayType : 0=Pluggable / 1=BuiltIn / X=Virtual
         * Bit6 - Pluggable : 0= TMDS Encoder / 1= DPMST / X=Builtin or Virtual
         * Encoder Index : Bit5~0 -> main : 0x1, cover : 0x2, external : 0x4~
         */
        if (mDisplayIdentifier.type == HWC_DISPLAY_PRIMARY) {
            *outPort = 0x80;
            *outPort |= 1 << mDisplayIdentifier.index;
        } else if (mDisplayIdentifier.type == HWC_DISPLAY_EXTERNAL) {
            *outPort = 0;
            *outPort |= 1 << (mDisplayIdentifier.index + 2);
        } else {
            *outPort = 0;
        }

        if (ioctl(mDisplayFd, EXYNOS_GET_EDID, &mEdidData) < 0) {
            ALOGI("%s: display(%d) cannot support EDID info", __func__,
                  mDisplayIdentifier.id);
            mEdidData.size = sizeof(EDID_SAMPLE);
            memcpy(mEdidData.edid_data, EDID_SAMPLE, sizeof(EDID_SAMPLE));
        }
        *outDataSize = mEdidData.size;
        return HWC2_ERROR_NONE;
    }

    memcpy(outData, mEdidData.edid_data, mEdidData.size);

    return HWC2_ERROR_NONE;
}

android_dataspace ExynosDisplayFbInterface::dataspaceFromConfig(const exynos_win_config_data &config) {
    return getRefinedDataspace(config.format.halFormat(), config.dataspace);
}

void ExynosDisplayFbInterface::setVsyncFd() {
    for (size_t i = 0; i < DISPLAY_COUNT; i++) {
        exynos_display_t display_t = AVAILABLE_DISPLAY_UNITS[i];
        if (display_t.type == HWC_DISPLAY_VIRTUAL)
            continue;
        if (getDisplayId(display_t.type, display_t.index) != mDisplayIdentifier.id)
            continue;

        char devname[MAX_DEV_NAME + 1];
        devname[MAX_DEV_NAME] = '\0';
        strncpy(devname, VSYNC_DEV_PREFIX, MAX_DEV_NAME);
        strlcat(devname, display_t.vsync_node_name, MAX_DEV_NAME);
        mVsyncFd = open(devname, O_RDONLY);
        if (mVsyncFd < 0)
            ALOGI("Failed to open vsync attribute at %s", devname);
    }
}

void ExynosDisplayFbInterface::alignDSCBlockSize(hwc_rect &merge_rect) {
    merge_rect.left = 0;
    merge_rect.right = mXres;
    merge_rect.top = 0;
    merge_rect.bottom = mYres;
}
