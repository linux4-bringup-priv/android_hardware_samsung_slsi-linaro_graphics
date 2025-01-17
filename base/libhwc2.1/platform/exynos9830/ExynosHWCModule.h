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

#ifndef ANDROID_EXYNOS_HWC_MODULE_H_
#define ANDROID_EXYNOS_HWC_MODULE_H_

#include "ExynosHWC.h"
#include "DeconHeader.h"
#include "ExynosHWCHelper.h"

#define VSYNC_DEV_PREFIX    "/sys/devices/platform/"
#define DECON_F_VSYNC_NODE  "19050000.decon_f/vsync"
#define DECON_F_FB_NODE     "/dev/graphics/fb0"
#define DECON_S_VSYNC_NODE  "19060000.decon_s/vsync"
#define DECON_S_FB_NODE     "/dev/graphics/fb1"
#define DECON_T_VSYNC_NODE  "19070000.decon_t/vsync"
#define DECON_T_FB_NODE     "/dev/graphics/fb2"
#define PSR_DEV_NAME        "19050000.decon_f/psr_info"
#define PSR_DEV_NAME_S      "19060000.decon_s/psr_info"

#define HIBER_EXIT_NODE_NAME    "/sys/devices/platform/19050000.decon_f/hiber_exit"
/* When use Brigntess, enable here */
#define BRIGHTNESS_NODE_BASE    "/sys/class/backlight/panel/brightness"
#define MAX_BRIGHTNESS_NODE_BASE    "/sys/class/backlight/panel/max_brightness"
/***********************************/

#define DP_LINK_NAME        "10ab0000.displayport"
#define DP_UEVENT_NAME      "change@/devices/platform/%s/extcon/extcon0"
#define DP_CABLE_STATE_NAME "/sys/devices/platform/%s/extcon/extcon0/cable.%d/state"

#define IDMA(x) static_cast<decon_idma_type>(x)

#define USES_SET_DISPLAY_MODE_IOCTL

enum {
    HWC_DISPLAY_NONE_BIT = 0
};

/*
 * pre_assign_info: all display_descriptors that want to reserve
 */
struct exynos_mpp_t {
    int physicalType;
    int logicalType;
    char name[16];
    uint32_t physical_index;
    uint32_t logical_index;
    uint32_t pre_assign_info;
};

const dpp_channel_map_t IDMA_CHANNEL_MAP[] = {
    /* GF physical index is switched to change assign order */
    {MPP_DPP_GF,     1, IDMA_GF0,   IDMA(0)},
    {MPP_DPP_GF,     0, IDMA_GF1,   IDMA(1)},
    {MPP_DPP_VG,     0, IDMA_VG0,   IDMA(2)},
    {MPP_DPP_VGS,    0, IDMA_VGS0,  IDMA(3)},
    {MPP_DPP_VGF,    0, IDMA_VGF0,  IDMA(4)},
    {MPP_DPP_VGRFS,  0, IDMA_VGRFS0,IDMA(5)},
    {MPP_P_TYPE_MAX, 0, ODMA_WB,    IDMA(6)}, // not idma but..
    {static_cast<mpp_phycal_type_t>(MAX_DECON_DMA_TYPE), 0, MAX_DECON_DMA_TYPE, IDMA(7)}
};

#define MAX_NAME_SIZE   32
struct exynos_display_t {
    uint32_t type;
    uint32_t index;
    char display_name[MAX_NAME_SIZE];
    char decon_node_name[MAX_NAME_SIZE];
    char vsync_node_name[MAX_NAME_SIZE];
};

#define PRIMARY_MAIN_EXTERNAL_WINCNT   2
#define EXTERNAL_MAIN_EXTERNAL_WINCNT  4

#define DISPLAY_MODE_MASK_LEN    8
#define DISPLAY_MODE_MASK_BIT    0xff
enum {
    DISPLAY_MODE_PRIMARY_MAIN = 0,  /* This is default mode */
    DISPLAY_MODE_EXTERNAL_MAIN,
    DISPLAY_MODE_NUM
};

/*
 * This is base window index of primary display for each display mode.
 * External display base window is always 0
 */
const uint32_t EXTERNAL_WINDOW_COUNT[] = {PRIMARY_MAIN_EXTERNAL_WINCNT, EXTERNAL_MAIN_EXTERNAL_WINCNT};

#define EXTERNAL_MAIN_DISPLAY_START_BIT (DISPLAY_MODE_MASK_LEN * DISPLAY_MODE_EXTERNAL_MAIN)
enum {
    EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT = 1 << (EXTERNAL_MAIN_DISPLAY_START_BIT + HWC_DISPLAY_PRIMARY),
    EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT = 1 << (EXTERNAL_MAIN_DISPLAY_START_BIT + HWC_DISPLAY_EXTERNAL),
    EXTERNAL_MAIN_DISPLAY_VIRTUAL_BIT = 1 << (EXTERNAL_MAIN_DISPLAY_START_BIT + HWC_DISPLAY_VIRTUAL),
};

const exynos_mpp_t AVAILABLE_OTF_MPP_UNITS[] = {
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF1", 0, 0, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF0", 1, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_VG, MPP_LOGICAL_DPP_VG, "DPP_VG0", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VGS, MPP_LOGICAL_DPP_VGS, "DPP_VGS0", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VGF, MPP_LOGICAL_DPP_VGF, "DPP_VGF0", 0, 0, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VGRFS, MPP_LOGICAL_DPP_VGRFS, "DPP_VGRFS0", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT}
};

const exynos_mpp_t AVAILABLE_M2M_MPP_UNITS[] = {
    {MPP_MSC, MPP_LOGICAL_MSC, "MSC0_PRI", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_EXT0", 0, 1, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_EXT1", 0, 2, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_YUV, "G2D0-YUV_PRI", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_YUV, "G2D0-YUV_PRI", 0, 1, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_YUV, "G2D0-YUV_EXT0", 0, 2, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_YUV, "G2D0-YUV_EXT1", 0, 3, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_RGB, "G2D0-RGB_PRI", 0, 4, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_RGB, "G2D0-RGB_EXT0", 0, 5, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_RGB, "G2D0-RGB_EXT1", 0, 6, HWC_DISPLAY_EXTERNAL_BIT|EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT},
    {MPP_G2D, MPP_LOGICAL_G2D_COMBO, "G2D0-COMBO_VIR", 0, 7, HWC_DISPLAY_VIRTUAL_BIT|EXTERNAL_MAIN_DISPLAY_VIRTUAL_BIT}
};

/* AVAILABLE_DISPLAY_UNITS's index is same with index of mDisplays. Many part of exynos HWC operates by order of
   mDisplays' index, so rules for deciding a AVAILABLE_DISPLAY_UNITS's index should be followed.
   - Rules
     1. Display's order is decided by descending order of display type defined in "hwcomposer_defs.h".
     2. If more than two displays have same display type, order is decided by descending order of
        exynos_display_t's index value.
   Scenario affcted by this order is as follows.
   - Scenario
     1. HWC's main API like as validateDisplay and presentDisplay should be called by inverse order of mDisplays' index.
     2. Process for preassigning M2MMPP resources should be called by inverse order of mDisplays' index.
     3. For the process about preassigning OTFMPP resources, display that do not use DPU like as virtual display
        should be alligned at the end. */
const exynos_display_t AVAILABLE_DISPLAY_UNITS[] = {
    {HWC_DISPLAY_PRIMARY, 0, "PrimaryDisplay",      DECON_F_FB_NODE, DECON_F_VSYNC_NODE},
#ifdef USES_DUAL_DISPLAY
    {HWC_DISPLAY_PRIMARY, 1, "PrimaryDisplay2",     DECON_S_FB_NODE, DECON_S_VSYNC_NODE},
    {HWC_DISPLAY_EXTERNAL, 0, "ExternalDisplay",    DECON_T_FB_NODE, DECON_T_VSYNC_NODE},
#else
    {HWC_DISPLAY_EXTERNAL, 0, "ExternalDisplay",    DECON_S_FB_NODE, DECON_S_VSYNC_NODE},
    {HWC_DISPLAY_EXTERNAL, 1, "ExternalDisplay2",   DECON_T_FB_NODE, DECON_T_VSYNC_NODE},
#endif
    {HWC_DISPLAY_VIRTUAL, 0, "VirtualDisplay", {}, {}},
};

#define DISPLAY_COUNT sizeof(AVAILABLE_DISPLAY_UNITS)/sizeof(exynos_display_t)

const uint32_t ATTRIBUTE_PRIORITY_LIST[] = {
    ATTRIBUTE_AFBC_NO_RESTRICTION,
    ATTRIBUTE_HDR10PLUS,
};

const exynos_mpp_t SHARED_RESOURCE_SET[2][2] = {
    {{MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF1", 0, 0, 0},
     {MPP_DPP_VGF, MPP_LOGICAL_DPP_VGF, "DPP_VGF0", 0, 0, 0}},
    {{MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF0", 1, 0, 0},
     {MPP_DPP_VGRFS, MPP_LOGICAL_DPP_VGRFS, "DPP_VGRFS0", 0, 0, 0}}
 };
const size_t product_layerAttributePriority[] = {ATTRIBUTE_DRM, ATTRIBUTE_HDR10PLUS, ATTRIBUTE_HDR10};

struct display_resource_info_t {
    uint32_t displayId;
    /* If this variable has a non-zero value, the display uses resource sets by defined value */
    uint32_t fix_setcnt;
    /* Small number have higher priority than bigger one.
     * Range of priority is from 0 to (NUM_OF_DISPLAY_NEEDED_OTFMPP -1) */
    uint32_t dpp_priority;
    uint32_t attr_priority[MAX_ATTRIBUTE_NUM];
};

const display_resource_info_t RESOURCE_INFO_TABLE[] = {
#ifdef USES_DUAL_DISPLAY
    {getDisplayId(HWC_DISPLAY_PRIMARY, 0), 0, 0, {0, 2}},
    {getDisplayId(HWC_DISPLAY_PRIMARY, 1), 0, 1, {1, 3}},
    {getDisplayId(HWC_DISPLAY_EXTERNAL, 0), 1, 2, {2, 0}},
    {getDisplayId(HWC_DISPLAY_EXTERNAL, 1), 1, 3, {3, 1}},
#else
    {getDisplayId(HWC_DISPLAY_PRIMARY, 0), 0, 0, {0, 2}},
    {getDisplayId(HWC_DISPLAY_EXTERNAL, 0), 1, 1, {1, 0}},
    {getDisplayId(HWC_DISPLAY_EXTERNAL, 1), 1, 2, {2, 1}},
#endif
};
#endif
