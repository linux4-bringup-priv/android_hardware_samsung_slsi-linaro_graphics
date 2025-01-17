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

#define VOTF_BUF_INDEX_MAX 15

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

struct vOtfInfo_t {
    mpp_phycal_type_t type;
    uint32_t index;
    decon_idma_type channel;
    unsigned int dma_idx;
    unsigned int trs_idx;
};

const vOtfInfo_t VOTF_INFO_MAP[] = {
    {MPP_DPP_G,      0, IDMA(L0), 0, 0},
    {MPP_DPP_VGRFS,  0, IDMA(L1), 0, 1},
    {MPP_DPP_GF,     0, IDMA(L2), 0, 2},
    {MPP_DPP_VGS,    0, IDMA(L3), 0, 3},
    {MPP_DPP_GF,     1, IDMA(L4), 0, 4},
    {MPP_DPP_VG,     0, IDMA(L5), 0, 5},
    {MPP_DPP_G,      1, IDMA(L6), 0, 6},
    {MPP_DPP_VGFS,   0, IDMA(L7), 0, 7},
    {MPP_DPP_G,      2, IDMA(L8), 1, 0},
    {MPP_DPP_VGRFS,  1, IDMA(L9), 1, 1},
    {MPP_DPP_GF,     2, IDMA(L10), 1, 2},
    {MPP_DPP_VGS,    1, IDMA(L11), 1, 3},
    {MPP_DPP_GF,     3, IDMA(L12), 1, 4},
    {MPP_DPP_VG,     1, IDMA(L13), 1, 5},
    {MPP_DPP_G,      3, IDMA(L14), 1, 6},
    {MPP_DPP_VGFS,   1, IDMA(L15), 1, 7}
};

const dpp_channel_map_t IDMA_CHANNEL_MAP[] = {
    /* GF physical index is switched to change assign order */
    {MPP_DPP_G,      0, IDMA_G0,     IDMA(L0)},
    {MPP_DPP_VGRFS,  0, IDMA_VGRFS0, IDMA(L1)},
    {MPP_DPP_GF,     0, IDMA_GF0,    IDMA(L2)},
    {MPP_DPP_VGS,    0, IDMA_VGS0,   IDMA(L3)},
    {MPP_DPP_GF,     1, IDMA_GF1,    IDMA(L4)},
    {MPP_DPP_VG,     0, IDMA_VG0,    IDMA(L5)},
    {MPP_DPP_G,      1, IDMA_G1,     IDMA(L6)},
    {MPP_DPP_VGFS,   0, IDMA_VGFS0,  IDMA(L7)},
    {MPP_DPP_G,      2, IDMA_G2,     IDMA(L8)},
    {MPP_DPP_VGRFS,  1, IDMA_VGRFS1, IDMA(L9)},
    {MPP_DPP_GF,     2, IDMA_GF2,    IDMA(L10)},
    {MPP_DPP_VGS,    1, IDMA_VGS1,   IDMA(L11)},
    {MPP_DPP_GF,     3, IDMA_GF3,    IDMA(L12)},
    {MPP_DPP_VG,     1, IDMA_VG1,    IDMA(L13)},
    {MPP_DPP_G,      3, IDMA_G3,     IDMA(L14)},
    {MPP_DPP_VGFS,   1, IDMA_VGFS1,  IDMA(L15)},
    /* Virtual 8K */
    /* IDMA indecates first thing for shared DPP */
    {MPP_DPP_VGS,    2, IDMA_VGS8K,   IDMA(L3)},
    {MPP_DPP_VGFS,   2, IDMA_VGFS8K,  IDMA(L7)},
    {MPP_DPP_VGRFS,  2, IDMA_VGRFS8K, IDMA(L1)},
    {MPP_P_TYPE_MAX, 0, ODMA_WB,     IDMA(WB)}, // not idma but..
    {static_cast<mpp_phycal_type_t>(MAX_DECON_DMA_TYPE), 0, MAX_DECON_DMA_TYPE, IDMA(WB+1)}
};

typedef struct virtual_dpp_map {
    mpp_logical_type_t logicalType;
    /* Physical index for virtual MPP */
    uint32_t physicalIndex;
    mpp_phycal_type_t physicalType;
    uint32_t physicalIndex1;
    uint32_t physicalIndex2;
    decon_idma_type channel1;
    decon_idma_type channel2;
    uint32_t    idma; // DECON_IDMA
} virtual_dpp_map_t;

const virtual_dpp_map_t VIRTUAL_CHANNEL_PAIR_MAP[] = {
    {MPP_LOGICAL_DPP_VGS8K,   2, MPP_DPP_VGS,   0, 1, IDMA(L3), IDMA(L11), IDMA_VGS8K},
    {MPP_LOGICAL_DPP_VGFS8K,  2, MPP_DPP_VGFS,  0, 1, IDMA(L7), IDMA(L15), IDMA_VGFS8K},
    {MPP_LOGICAL_DPP_VGRFS8K, 2, MPP_DPP_VGRFS, 0, 1, IDMA(L1), IDMA(L9),  IDMA_VGRFS8K}
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

#define PRIMARY_MAIN_EXTERNAL_WINCNT   3
#define EXTERNAL_MAIN_EXTERNAL_WINCNT  4
#define PRIMARY_MAIN_VIRTUAL_WINCNT   5

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

enum {
    HWC_DISPLAY_EXTERNAL2_BIT = 1 << (SECOND_DISPLAY_START_BIT + HWC_DISPLAY_EXTERNAL),
};

const exynos_mpp_t AVAILABLE_OTF_MPP_UNITS[] = {
    {MPP_DPP_G, MPP_LOGICAL_DPP_G, "DPP_G3", 3, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_G, MPP_LOGICAL_DPP_G, "DPP_G2", 2, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_G, MPP_LOGICAL_DPP_G, "DPP_G1", 1, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_G, MPP_LOGICAL_DPP_G, "DPP_G0", 0, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF3", 3, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF2", 2, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF1", 1, 0, HWC_DISPLAY_EXTERNAL2_BIT},
    {MPP_DPP_GF, MPP_LOGICAL_DPP_GF, "DPP_GF0", 0, 0, HWC_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VG, MPP_LOGICAL_DPP_VG, "DPP_VG1", 1, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_VG, MPP_LOGICAL_DPP_VG, "DPP_VG0", 0, 0, HWC_DISPLAY_PRIMARY_BIT},
    {MPP_DPP_VGS, MPP_LOGICAL_DPP_VGS, "DPP_VGS1", 1, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL2_BIT},
    {MPP_DPP_VGS, MPP_LOGICAL_DPP_VGS, "DPP_VGS0", 0, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL2_BIT},
    {MPP_DPP_VGFS, MPP_LOGICAL_DPP_VGFS, "DPP_VGFS1", 1, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VGFS, MPP_LOGICAL_DPP_VGFS, "DPP_VGFS0", 0, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL_BIT},
   {MPP_DPP_VGRFS, MPP_LOGICAL_DPP_VGRFS, "DPP_VGRFS1", 1, 0, HWC_DISPLAY_VIRTUAL_BIT},
    {MPP_DPP_VGRFS, MPP_LOGICAL_DPP_VGRFS, "DPP_VGRFS0", 0, 0, HWC_DISPLAY_PRIMARY_BIT},
    /* virtual 8K */
    {MPP_DPP_VGS, MPP_LOGICAL_DPP_VGS8K, "DPP_VGS8K", 2, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL2_BIT},
    {MPP_DPP_VGFS, MPP_LOGICAL_DPP_VGFS8K, "DPP_VGFS8K", 2, 0, HWC_DISPLAY_VIRTUAL_BIT | HWC_DISPLAY_EXTERNAL_BIT},
    {MPP_DPP_VGRFS, MPP_LOGICAL_DPP_VGRFS8K, "DPP_VGRFS8K", 2, 0, HWC_DISPLAY_VIRTUAL_BIT}
};

const exynos_mpp_t AVAILABLE_M2M_MPP_UNITS[] = {
    {MPP_MSC, MPP_LOGICAL_MSC, "MSC0_PRI", 0, 0, HWC_DISPLAY_PRIMARY_BIT|EXTERNAL_MAIN_DISPLAY_PRIMARY_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_VIR0", 0, 1, HWC_DISPLAY_VIRTUAL_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_VIR1", 0, 2, HWC_DISPLAY_VIRTUAL_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_EXT0", 0, 3, HWC_DISPLAY_EXTERNAL_BIT},
    {MPP_MSC, MPP_LOGICAL_MSC_YUV, "MSC0_EXT1", 0, 4, HWC_DISPLAY_EXTERNAL2_BIT},
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
#ifdef USES_DUAL_DISPLAY
    {HWC_DISPLAY_PRIMARY, 1, "PrimaryDisplay2",     DECON1_FB_NODE, DECON1_VSYNC_NODE},
#endif
    {HWC_DISPLAY_EXTERNAL, 0, "ExternalDisplay",    DECON2_FB_NODE, DECON2_VSYNC_NODE},
    {HWC_DISPLAY_EXTERNAL, 1, "ExternalDisplay2",   DECON3_FB_NODE, DECON3_VSYNC_NODE},
    {HWC_DISPLAY_VIRTUAL, 0, "VirtualDisplay", DECON1_FB_NODE, {}},
};

#define DISPLAY_COUNT sizeof(AVAILABLE_DISPLAY_UNITS)/sizeof(exynos_display_t)

const uint32_t ATTRIBUTE_PRIORITY_LIST[] = {
    ATTRIBUTE_AFBC_NO_RESTRICTION,
    ATTRIBUTE_HDR10PLUS,
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
