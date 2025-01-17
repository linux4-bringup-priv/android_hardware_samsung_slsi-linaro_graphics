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
#define DECON0_VSYNC_NODE  "19f00000.decon_0/vsync"
#define DECON0_FB_NODE     "/dev/graphics/fb0"
#define DECON1_VSYNC_NODE  "19f01000.decon_1/vsync"
#define DECON1_FB_NODE     "/dev/graphics/fb1"
#define DECON2_VSYNC_NODE  "19f02000.decon_2/vsync"
#define DECON2_FB_NODE     "/dev/graphics/fb2"
#define DECON3_VSYNC_NODE  "19f03000.decon_3/vsync"
#define DECON3_FB_NODE     "/dev/graphics/fb3"
#define PSR_DEV_NAME        "19f00000.decon_0/psr_info"
#define PSR_DEV_NAME_S      "19f01000.decon_1/psr_info"
#define USE_DPU_SET_CONFIG

#define HIBER_EXIT_NODE_NAME    "/sys/devices/platform/19f00000.drmdecon/hiber_exit"
/* When use Brigntess, enable here */
//#define BRIGHTNESS_NODE_BASE    "/sys/devices/virtual/backlight/panel_0/brightness"
//#define MAX_BRIGHTNESS_NODE_BASE    "/sys/devices/virtual/backlight/panel_0/max_brightness"

#define DP_LINK_NAME        "10ab0000.displayport"
#define DP_UEVENT_NAME      "change@/devices/platform/%s/extcon/extcon0"
#define DP_CABLE_STATE_NAME "/sys/devices/platform/%s/extcon/extcon0/cable.%d/state"

#define IDMA(x) static_cast<decon_idma_type>(x)

#define DEFAULT_MPP_DST_YUV_FORMAT HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_SBWC
#define MSC_CLOCK   800000

#define VPP_CLOCK       663000000
#define VPP_PIXEL_PER_CLOCK 2

#define VPP_MARGIN 1.1
#define VPP_DISP_FACTOR 1.0

enum {
    HWC_DISPLAY_NONE_BIT = 0
};

enum {
    L0 = 0, L1, L2, L3, L4, L5, L6, L7, L8, L9, L10, L11, L12, L13, L14, L15, WB
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
};

#define MAX_NAME_SIZE   32
struct exynos_display_t {
    uint32_t type;
    uint32_t index;
    char display_name[MAX_NAME_SIZE];
    char decon_node_name[MAX_NAME_SIZE];
    char vsync_node_name[MAX_NAME_SIZE];
};

/******** Description about display bit ********/
/*   DISPLAY BIT = 1 << (DISPLAY_MODE_MASK_LEN * display mode
 *                       + SECOND_DISPLAY_START_BIT * display index
 *                       + display type);
 *   ex) HWC_DISPLAY_EXTERNAL2_BIT = 1 << (DISPLAY_MODE_MASK_LEN * display mode(0)
 *                                         + SECOND_DISPLAY_START_BIT * display index(1)
 *                                         + displayy type(1))
 *                                 = 1 << 5
 *   PRIMARY MAIN MODE :
 *      0 bit : HWC_DISPLAY_PRIMARY_BIT,
 *      1 bit : HWC_DISPLAY_EXTERNAL_BIT,
 *      2 bit : HWC_DISPLAY_VIRTUAL_BIT,
 *      5 bit : HWC_DISPLAY_EXTERNAL2_BIT,
 *   EXTERNAL MAIN MODE :
 *      8 bit : EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT,
 *      9 bit : EXTERNAL_MAIN_DISPLAY_EXTERNAL_BIT,
 *      10 bit : EXTERNAL_MAIN_DISPLAY_VIRTUAL_BIT,
 ***************************************************/

#define PRIMARY_MAIN_EXTERNAL_WINCNT   0
#define EXTERNAL_MAIN_EXTERNAL_WINCNT  0
#define PRIMARY_MAIN_VIRTUAL_WINCNT   0

#define DISPLAY_MODE_MASK_LEN    8
#define DISPLAY_MODE_MASK_BIT    0xff
#define SECOND_DISPLAY_START_BIT   4

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
    {MPP_DPP_G, MPP_LOGICAL_DPP_G, "DPP_G0", 0, 0, HWC_DISPLAY_PRIMARY_BIT},
};

const exynos_mpp_t AVAILABLE_M2M_MPP_UNITS[] = {
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
    {HWC_DISPLAY_PRIMARY, 0, "PrimaryDisplay",      DECON0_FB_NODE, DECON0_VSYNC_NODE},
    {HWC_DISPLAY_EXTERNAL, 0, "ExternalDisplay",    DECON2_FB_NODE, DECON2_VSYNC_NODE},
    {HWC_DISPLAY_VIRTUAL, 0, "VirtualDisplay", DECON1_FB_NODE, {}},
};

#define DISPLAY_COUNT sizeof(AVAILABLE_DISPLAY_UNITS)/sizeof(exynos_display_t)

struct display_resource_info_t {
    uint32_t displayId;
    /* If this variable has a non-zero value, the display uses resource sets by defined value */
    uint32_t fix_setcnt;
    /* Small number have higher priority than bigger one.
     * Range of priority is from 0 to (NUM_OF_DISPLAY_NEEDED_OTFMPP -1) */
    uint32_t dpp_priority;
    uint32_t attr_priority[MAX_ATTRIBUTE_NUM];
};

#endif
