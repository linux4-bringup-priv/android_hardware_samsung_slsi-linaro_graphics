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

#ifndef EXYNOS_RESOURCE_RESTRICTION_H_
#define EXYNOS_RESOURCE_RESTRICTION_H_

#include "ExynosHWCModule.h"
/*******************************************************************
 * Structures for restrictions
 * ****************************************************************/
#define RESTRICTION_NONE 0

#define USE_MODULE_ATTR

/* Basic supported features */
static feature_support_t feature_table[] =
{
    {MPP_DPP_G,
        MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM
    },

    {MPP_DPP_GF,
        MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM
    },

    {MPP_DPP_VG,
        MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM
    },

    {MPP_DPP_VGS,
        MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE | MPP_ATTR_DIM
    },

    {MPP_DPP_VGF,
        MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_DIM
    },

    {MPP_DPP_VGFS,
        MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE | MPP_ATTR_DIM
    },

    {MPP_DPP_VGRFS,
        MPP_ATTR_AFBC | MPP_ATTR_BLOCK_MODE | MPP_ATTR_WINDOW_UPDATE | MPP_ATTR_SCALE |
        MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90 | MPP_ATTR_DIM
    },

    {MPP_MSC,
        MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90
    },

    {MPP_G2D,
        MPP_ATTR_AFBC | MPP_ATTR_FLIP_H | MPP_ATTR_FLIP_V | MPP_ATTR_ROT_90 | MPP_ATTR_USE_CAPA | MPP_ATTR_DIM
    }
};

/**************************************************************************************
 * HAL_PIXEL_FORMATs
enum {
    HAL_PIXEL_FORMAT_RGBA_8888          = 1,
    HAL_PIXEL_FORMAT_RGBX_8888          = 2,
    HAL_PIXEL_FORMAT_RGB_888            = 3,
    HAL_PIXEL_FORMAT_RGB_565            = 4,
    HAL_PIXEL_FORMAT_BGRA_8888          = 5,
    HAL_PIXEL_FORMAT_YV12   = 0x32315659, // YCrCb 4:2:0 Planar
    HAL_PIXEL_FORMAT_YCbCr_422_SP       = 0x10, // NV16
    HAL_PIXEL_FORMAT_YCrCb_420_SP       = 0x11, // NV21
    HAL_PIXEL_FORMAT_YCbCr_422_I        = 0x14, // YUY2
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M       = 0x101,
    HAL_PIXEL_FORMAT_EXYNOS_CbYCrY_422_I        = 0x103,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M      = 0x105,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_422_SP        = 0x106,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED= 0x107,
    HAL_PIXEL_FORMAT_EXYNOS_ARGB_8888           = 0x108,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_422_I         = 0x116,
    HAL_PIXEL_FORMAT_EXYNOS_CrYCbY_422_I        = 0x118,
    HAL_PIXEL_FORMAT_EXYNOS_YV12_M              = 0x11C,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M      = 0x11D,
    HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL = 0x11E,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P         = 0x11F,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP        = 0x120,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV = 0x121,
    // contiguous(single fd) custom formats
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_PN        = 0x122,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN       = 0x123,
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_TILED = 0x124,
    // 10-bit format (8bit + separated 2bit)
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B = 0x125,
    // 10-bit contiguous(single fd, 8bit + separated 2bit) custom formats
    HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B  = 0x126,
};
*************************************************************************************/

const restriction_key_t restriction_format_table[] =
{
    {MPP_DPP_G, NODE_NONE, HAL_PIXEL_FORMAT_RGB_565, 0},
    {MPP_DPP_G, NODE_NONE, HAL_PIXEL_FORMAT_RGBA_8888, 0},
    {MPP_DPP_G, NODE_NONE, HAL_PIXEL_FORMAT_RGBX_8888, 0},
    {MPP_DPP_G, NODE_NONE, HAL_PIXEL_FORMAT_BGRA_8888, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_RGB_565, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_RGBA_8888, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_RGBX_8888, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_BGRA_8888, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B, 0},
    {MPP_DPP_VG, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_RGB_565, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_RGB_888, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_RGBA_8888, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_RGBX_8888, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_BGRA_8888, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_PRIV, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_TILED, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M_S10B, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_S10B, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_YCrCb_420_SP, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_YV12, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YV12_M, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_P_M, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_PN, 0},
    {MPP_MSC, NODE_NONE, HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SPN_TILED, 0}
};

const restriction_size_element restriction_size_table_rgb[] =
{
    {{MPP_DPP_G, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
     {  1,  1,  65535,  8191,   16, 16,   1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_DPP_VG, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
     {  1,  1,  65535,  8191,   16, 16,   1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_MSC, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
     {  16,  8,  8192,  8192,  16, 16,  1,  1,  8192,   8192,   16, 16,  1,  1,  1,  1}},
    {{MPP_DPP_G, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
     {  1,  1,  65535,  8191,   16, 16,   1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_DPP_VG, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
     {  1,  1,  65535,  8191,   16, 16,   1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_MSC, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
     {  16,  8,  8192,  8192,  4, 4,  1,  1,  8192,  8192,   4, 4,  1,  1,  1,  1}}
};

const restriction_size_element restriction_size_table_yuv[] =
{
    {{MPP_DPP_G, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
        {  1,  1,  65534,  8190,   32, 32,  2,  2,  4096,   4096,   32, 32,  2,  2,  2,  2}},
    {{MPP_DPP_VG, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
        {  1,  1,  65534,  8190,   32, 32,  2,  2,  4096,   4096,   32, 32,  2,  2,  2,  2}},
    {{MPP_MSC, NODE_SRC, HAL_PIXEL_FORMAT_NONE, 0},
        {  16,  8,  8192, 8192,  16, 16,  2,  2,  8192,   8192,   16, 16,  2,  2,  2,  2}},
    {{MPP_DPP_G, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
        {  1,  1,  65535,  8191,   16, 16,  1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_DPP_VG, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
        {  1,  1,  65535,  8191,   16, 16,  1,  1,  4096,   4096,   16,  16,  1,  1,  1,  1}},
    {{MPP_MSC, NODE_DST, HAL_PIXEL_FORMAT_NONE, 0},
        {  16,  8,  8192,  8192,  4, 4,  2,  2,  8192,   8192,   4, 4,  2,  2,  2,  2}}
};

const restriction_table_element restriction_tables[RESTRICTION_MAX] =
{
    {RESTRICTION_RGB, restriction_size_table_rgb, sizeof(restriction_size_table_rgb)/sizeof(restriction_size_element)},
    {RESTRICTION_YUV, restriction_size_table_yuv, sizeof(restriction_size_table_yuv)/sizeof(restriction_size_element)}
};

static ppc_table ppc_table_map = {
    {PPC_IDX(MPP_MSC,PPC_FORMAT_YUV420,PPC_ROT_NO), {0.8, }},
    {PPC_IDX(MPP_MSC,PPC_FORMAT_YUV420,PPC_ROT), {0.8, }},

    {PPC_IDX(MPP_MSC,PPC_FORMAT_YUV422,PPC_ROT_NO), {0.8, }},
    {PPC_IDX(MPP_MSC,PPC_FORMAT_YUV422,PPC_ROT), {0.8, }},

    {PPC_IDX(MPP_MSC,PPC_FORMAT_RGB32,PPC_ROT_NO), {0.8, }},
    {PPC_IDX(MPP_MSC,PPC_FORMAT_RGB32,PPC_ROT), {0.8, }}
};

#endif
