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
#include <utils/Errors.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <utils/CallStack.h>
#include <android/sync.h>
#include <hardware/exynos/ion.h>
#include "ExynosHWCHelper.h"
#include "ExynosHWCDebug.h"
#include "ExynosHWC.h"
#include "ExynosLayer.h"
#include "exynos_sync.h"
#include "videodev2_exynos_media.h"
#include "VendorVideoAPI.h"
#include "ExynosResourceRestriction.h"
#include "ExynosGraphicBuffer.h"

using vendor::graphics::BufferUsage;
using vendor::graphics::ExynosGraphicBufferMeta;
using vendor::graphics::ExynosGraphicBufferUsage;

#define AFBC_MAGIC 0xafbc

#if defined(DISABLE_HWC_DEBUG)
#define FT_LOGD(...)
#define FT_LOGI(...)
#else
#define FT_LOGD(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer >= 2)           \
            ALOGD("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }
#define FT_LOGI(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer >= 1)           \
            ALOGD("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }
#endif
#define FT_LOGE(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer > 0)            \
            ALOGE("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }

using vendor::graphics::BufferUsage;
using vendor::graphics::ExynosGraphicBufferMeta;
using vendor::graphics::ExynosGraphicBufferUsage;

extern struct exynos_hwc_control exynosHWCControl;

uint32_t getDrmMode(uint64_t flags) {
#ifdef GRALLOC_VERSION1
    if (flags & BufferUsage::PROTECTED) {
        if (flags & ExynosGraphicBufferUsage::PRIVATE_NONSECURE)
            return NORMAL_DRM;
        else
            return SECURE_DRM;
    }
#else
    if (flags & BufferUsage::PROTECTED) {
        if (flags & BufferUsage::PRIVATE_NONSECURE)
            return NORMAL_DRM;
        else
            return SECURE_DRM;
    }
#endif
    return NO_DRM;
}

uint32_t is8KVideo(const buffer_handle_t handle, hwc_frect_t srcCrop) {
    if (isFormatYUV(ExynosGraphicBufferMeta::get_format(handle))) {
        if ((srcCrop.right - srcCrop.left) > MIN_WIDTH_USING_VIRTUAL_8K) {
            return 1;
        }
    }
    return 0;
}

uint32_t getDrmMode(const buffer_handle_t handle) {
#ifdef GRALLOC_VERSION1
    uint64_t usage = ExynosGraphicBufferMeta::get_usage(handle);
    if (usage & BufferUsage::PROTECTED) {
        if (usage & ExynosGraphicBufferUsage::PRIVATE_NONSECURE)
            return NORMAL_DRM;
        else
            return SECURE_DRM;
    }
#else
    if (handle->flags & BufferUsage::PROTECTED) {
        if (handle->flags & ExynosGraphicBufferUsage::PRIVATE_NONSECURE)
            return NORMAL_DRM;
        else
            return SECURE_DRM;
    }
#endif
    return NO_DRM;
}

bool isAFBCCompressed(const buffer_handle_t handle) {
    if (handle != NULL) {
        return ExynosGraphicBufferMeta::is_afbc(handle);
    }

    return false;
}

bool isSAJCCompressed(const buffer_handle_t handle) {
    if (handle != NULL) {
        return ExynosGraphicBufferMeta::is_sajc(handle);
    }

    return false;
}

android_dataspace colorModeToDataspace(android_color_mode_t mode) {
    android_dataspace dataSpace = HAL_DATASPACE_UNKNOWN;
    switch (mode) {
    case HAL_COLOR_MODE_STANDARD_BT601_625:
        dataSpace = HAL_DATASPACE_STANDARD_BT601_625;
        break;
    case HAL_COLOR_MODE_STANDARD_BT601_625_UNADJUSTED:
        dataSpace = HAL_DATASPACE_STANDARD_BT601_625_UNADJUSTED;
        break;
    case HAL_COLOR_MODE_STANDARD_BT601_525:
        dataSpace = HAL_DATASPACE_STANDARD_BT601_525;
        break;
    case HAL_COLOR_MODE_STANDARD_BT601_525_UNADJUSTED:
        dataSpace = HAL_DATASPACE_STANDARD_BT601_525_UNADJUSTED;
        break;
    case HAL_COLOR_MODE_STANDARD_BT709:
        dataSpace = HAL_DATASPACE_STANDARD_BT709;
        break;
    case HAL_COLOR_MODE_DCI_P3:
        dataSpace = HAL_DATASPACE_DCI_P3;
        break;
    case HAL_COLOR_MODE_ADOBE_RGB:
        dataSpace = HAL_DATASPACE_ADOBE_RGB;
        break;
    case HAL_COLOR_MODE_DISPLAY_P3:
        dataSpace = HAL_DATASPACE_DISPLAY_P3;
        break;
    case HAL_COLOR_MODE_SRGB:
        dataSpace = HAL_DATASPACE_V0_SRGB;
        break;
    case HAL_COLOR_MODE_NATIVE:
        dataSpace = HAL_DATASPACE_UNKNOWN;
        break;
    default:
        break;
    }
    return dataSpace;
}

enum decon_blending halBlendingToDpuBlending(int32_t blending) {
    switch (blending) {
    case HWC2_BLEND_MODE_NONE:
        return DECON_BLENDING_NONE;
    case HWC2_BLEND_MODE_PREMULTIPLIED:
        return DECON_BLENDING_PREMULT;
    case HWC2_BLEND_MODE_COVERAGE:
        return DECON_BLENDING_COVERAGE;

    default:
        return DECON_BLENDING_MAX;
    }
}

enum dpp_rotate halTransformToDpuRot(uint32_t halTransform) {
    switch (halTransform) {
    case HAL_TRANSFORM_FLIP_H:
        return DPP_ROT_YFLIP;
    case HAL_TRANSFORM_FLIP_V:
        return DPP_ROT_XFLIP;
    case HAL_TRANSFORM_ROT_180:
        return DPP_ROT_180;
    case HAL_TRANSFORM_ROT_90:
        return DPP_ROT_90;
    case (HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_H):
        /*
         * HAL: HAL_TRANSFORM_FLIP_H -> HAL_TRANSFORM_ROT_90
         * DPP: ROT_90 -> XFLIP
         */
        return DPP_ROT_90_XFLIP;
    case (HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_V):
        /*
         * HAL: HAL_TRANSFORM_FLIP_V -> HAL_TRANSFORM_ROT_90
         * DPP: ROT_90 -> YFLIP
         */
        return DPP_ROT_90_YFLIP;
    case HAL_TRANSFORM_ROT_270:
        return DPP_ROT_270;
    default:
        return DPP_ROT_NORMAL;
    }
}

uint64_t halTransformToDrmRot(uint32_t halTransform) {
    switch (halTransform) {
    case HAL_TRANSFORM_FLIP_H:
        return DRM_MODE_REFLECT_Y | DRM_MODE_ROTATE_0;
    case HAL_TRANSFORM_FLIP_V:
        return DRM_MODE_REFLECT_X | DRM_MODE_ROTATE_0;
    case HAL_TRANSFORM_ROT_180:
        return DRM_MODE_ROTATE_180;
    case HAL_TRANSFORM_ROT_90:
        return DRM_MODE_ROTATE_90;
    case (HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_H):
        /*
         * HAL: HAL_TRANSFORM_FLIP_H -> HAL_TRANSFORM_ROT_90
         * DPP: ROT_90 -> XFLIP
         */
        return (DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_X);
    case (HAL_TRANSFORM_ROT_90 | HAL_TRANSFORM_FLIP_V):
        /*
         * HAL: HAL_TRANSFORM_FLIP_V -> HAL_TRANSFORM_ROT_90
         * DPP: ROT_90 -> YFLIP
         */
        return (DRM_MODE_ROTATE_90 | DRM_MODE_REFLECT_Y);
    case HAL_TRANSFORM_ROT_270:
        return DRM_MODE_ROTATE_270;
    default:
        return DRM_MODE_ROTATE_0;
    }
}

void convertSizeRestriction(const dpp_restriction &restriction, restriction_size &src,
                            restriction_size &dst) {
    src.maxDownScale = restriction.scale_down;
    src.maxUpScale = restriction.scale_up;
    src.maxFullWidth = restriction.src_f_w.max;
    src.maxFullHeight = restriction.src_f_h.max;
    src.minFullWidth = restriction.src_f_w.min;
    src.minFullHeight = restriction.src_f_h.min;
    src.fullWidthAlign = restriction.src_x_align;
    src.fullHeightAlign = restriction.src_y_align;
    src.maxCropWidth = restriction.src_w.max;
    src.maxCropHeight = restriction.src_h.max;
    src.minCropWidth = restriction.src_w.min;
    src.minCropHeight = restriction.src_h.min;
    src.cropXAlign = restriction.src_x_align;
    src.cropYAlign = restriction.src_y_align;
    src.cropWidthAlign = restriction.blk_x_align;
    src.cropHeightAlign = restriction.blk_y_align;

    dst.maxDownScale = restriction.scale_down;
    dst.maxUpScale = restriction.scale_up;
    dst.maxFullWidth = restriction.dst_f_w.max;
    dst.maxFullHeight = restriction.dst_f_h.max;
    dst.minFullWidth = restriction.dst_f_w.min;
    dst.minFullHeight = restriction.dst_f_h.min;
    dst.fullWidthAlign = restriction.dst_x_align;
    dst.fullHeightAlign = restriction.dst_y_align;
    dst.maxCropWidth = restriction.dst_w.max;
    dst.maxCropHeight = restriction.dst_h.max;
    dst.minCropWidth = restriction.dst_w.min;
    dst.minCropHeight = restriction.dst_h.min;
    dst.cropXAlign = restriction.dst_x_align;
    dst.cropYAlign = restriction.dst_y_align;
    dst.cropWidthAlign = restriction.blk_x_align;
    dst.cropHeightAlign = restriction.blk_y_align;
}

void convertYUVSizeRestriction(const dpp_restriction &restriction, restriction_size &src,
                               restriction_size &dst) {
    src.minCropWidth = restriction.src_w.min * 2;
    src.minCropHeight = restriction.src_h.min * 2;
    src.fullWidthAlign = max(restriction.src_x_align, YUV_CHROMA_H_SUBSAMPLE);
    src.fullHeightAlign = max(restriction.src_y_align, YUV_CHROMA_V_SUBSAMPLE);
    src.cropXAlign = max(restriction.src_x_align, YUV_CHROMA_H_SUBSAMPLE);
    src.cropYAlign = max(restriction.src_y_align, YUV_CHROMA_V_SUBSAMPLE);
    src.cropWidthAlign = max(restriction.blk_x_align, YUV_CHROMA_H_SUBSAMPLE);
    src.cropHeightAlign = max(restriction.blk_y_align, YUV_CHROMA_V_SUBSAMPLE);

    dst.minCropWidth = restriction.dst_w.min * 2;
    dst.minCropHeight = restriction.dst_h.min * 2;
    dst.fullWidthAlign = max(restriction.dst_x_align, YUV_CHROMA_H_SUBSAMPLE);
    dst.fullHeightAlign = max(restriction.dst_y_align, YUV_CHROMA_V_SUBSAMPLE);
    dst.cropXAlign = max(restriction.dst_x_align, YUV_CHROMA_H_SUBSAMPLE);
    dst.cropYAlign = max(restriction.dst_y_align, YUV_CHROMA_V_SUBSAMPLE);
    dst.cropWidthAlign = max(restriction.blk_x_align, YUV_CHROMA_H_SUBSAMPLE);
    dst.cropHeightAlign = max(restriction.blk_y_align, YUV_CHROMA_V_SUBSAMPLE);
}

void dumpHandle(uint32_t type, buffer_handle_t h) {
    if (h == NULL)
        return;

    ExynosGraphicBufferMeta gmeta(h);

    HDEBUGLOGD(type, "\t\tformat = %d, width = %u, height = %u, stride = %u, vstride = %u",
               gmeta.format, gmeta.width, gmeta.height, gmeta.stride, gmeta.vstride);
}

void dumpExynosImage(uint32_t type, exynos_image &img) {
    if (!hwcCheckDebugMessages(type))
        return;
    ALOGD("\tbufferHandle: %p, fullWidth: %d, fullHeight: %d, x: %d, y: %d, w: %d, h: %d, format: %s",
          img.bufferHandle, img.fullWidth, img.fullHeight, img.x, img.y, img.w, img.h, img.exynosFormat.name().c_str());
    ALOGD("\tusageFlags: 0x%" PRIx64 ", layerFlags: 0x%8x, acquireFenceFd: %d, releaseFenceFd: %d",
          img.usageFlags, img.layerFlags, img.acquireFenceFd, img.releaseFenceFd);
    ALOGD("\tdataSpace(%d), blending(%d), transform(0x%2x), compressionType(%8x)",
          img.dataSpace, img.blending, img.transform, img.compressionInfo.type);
}

void dumpExynosImage(String8 &result, exynos_image &img) {
    result.appendFormat("\tbufferHandle: %p, fullWidth: %d, fullHeight: %d, x: %d, y: %d, w: %d, h: %d, format: %s\n",
                        img.bufferHandle, img.fullWidth, img.fullHeight, img.x, img.y, img.w, img.h, img.exynosFormat.name().c_str());
    result.appendFormat("\tusageFlags: 0x%" PRIx64 ", layerFlags: 0x%8x, acquireFenceFd: %d, releaseFenceFd: %d\n",
                        img.usageFlags, img.layerFlags, img.acquireFenceFd, img.releaseFenceFd);
    result.appendFormat("\tdataSpace(%d), blending(%d), transform(0x%2x), compressionType(%8x)\n",
                        img.dataSpace, img.blending, img.transform, img.compressionInfo.type);
    if (img.bufferHandle != NULL) {
        ExynosGraphicBufferMeta gmeta(img.bufferHandle);
        result.appendFormat("\tbuffer's stride: %d, %d\n", gmeta.stride, gmeta.vstride);
    }
}

bool isSrcCropFloat(hwc_frect &frect) {
    return (frect.left != (int)frect.left) ||
           (frect.top != (int)frect.top) ||
           (frect.right != (int)frect.right) ||
           (frect.bottom != (int)frect.bottom);
}

bool hasHdrInfo(exynos_image &img) {
    uint32_t dataSpace = img.dataSpace;

    /* By reference Layer's dataspace */
    uint32_t standard = (dataSpace & HAL_DATASPACE_STANDARD_MASK);
    uint32_t transfer = (dataSpace & HAL_DATASPACE_TRANSFER_MASK);

    if ((standard == HAL_DATASPACE_STANDARD_BT2020) ||
        (standard == HAL_DATASPACE_STANDARD_BT2020_CONSTANT_LUMINANCE) ||
        (standard == HAL_DATASPACE_STANDARD_DCI_P3)) {
        if ((transfer == HAL_DATASPACE_TRANSFER_ST2084) ||
            (transfer == HAL_DATASPACE_TRANSFER_HLG))
            return true;
        else
            return false;
    }

    return false;
}

bool hasHdrInfo(android_dataspace dataSpace) {
    exynos_image img;
    img.dataSpace = dataSpace;
    return hasHdrInfo(img);
}

bool hasHdr10Plus(exynos_image &img) {
    /* TODO Check layer has hdr10 and dynamic metadata here */
    return (img.metaType & VIDEO_INFO_TYPE_HDR_DYNAMIC) ? true : false;
}

void adjustRect(hwc_rect_t &rect, int32_t width, int32_t height) {
    if (rect.left < 0)
        rect.left = 0;
    if (rect.left > width)
        rect.left = width;
    if (rect.top < 0)
        rect.top = 0;
    if (rect.top > height)
        rect.top = height;
    if (rect.right < rect.left)
        rect.right = rect.left;
    if (rect.right > width)
        rect.right = width;
    if (rect.bottom < rect.top)
        rect.bottom = rect.top;
    if (rect.bottom > height)
        rect.bottom = height;
}

int pixel_align_down(int x, int a) {
    if ((a != 0) && ((x % a) != 0)) {
        int ret = ((x) - (x % a));
        if (ret < 0)
            ret = 0;
        return ret;
    }
    return x;
}

int getBufLength(buffer_handle_t handle, uint32_t planerNum, size_t *length, int format, uint32_t width, uint32_t height) {
    uint32_t bufferNumber = getBufferNumOfFormat(format);
    if ((handle == 0) || (bufferNumber == 0) || (bufferNumber > planerNum))
        return -EINVAL;

    ExynosGraphicBufferMeta gmeta(handle);

    switch (bufferNumber) {
    case 1:
        length[0] = gmeta.size;
        break;
    case 2:
        HDEBUGLOGD(eDebugMPP, "-- %s x : %d y : %d format : %d", __func__, width, height, format);
        length[0] = gmeta.size;
        length[1] = gmeta.size1;
        HDEBUGLOGD(eDebugMPP, "Y size : %zu CbCr size : %zu", length[0], length[1]);
        break;
    case 3:
        length[0] = width * height;
        length[1] = (length[0] / 4);
        length[2] = (length[0] / 4);
        break;
    }
    return NO_ERROR;
}

void printBufLength(buffer_handle_t handle, uint32_t planerNum, size_t *length,
                    int format, uint32_t width, uint32_t height) {
    uint32_t bufferNumber = getBufferNumOfFormat(format);
    if (getBufLength(handle, planerNum, length, format, width, height) < 0) {
        ALOGD("%s:: handle(%p), bufferNumber(%d), planerNum(%d)",
              __func__, handle, bufferNumber, planerNum);
        return;
    }

    String8 lengthStr;
    for (uint32_t i = 0; i < bufferNumber; i++)
        lengthStr.appendFormat("%zu ", length[i]);
    ALOGD("format: 0x%8x, length(%s)", format, lengthStr.c_str());
}

int hwcFdClose(int fd) {
    if (fd >= 3)
        close(fd);
    else if (fd == -1) {
        HDEBUGLOGD(eDebugFence, "%s : Fd is -1", __func__);
    } else {
        ALOGW("%s : Fd:%d is less than 3", __func__, fd);
        hwc_print_stack();
    }
    return -1;
}

int hwc_print_stack() {
#if 0
    CallStack stack(LOG_TAG);
    stack.update();
    stack.log("HWCException", ANDROID_LOG_ERROR, "HWCException");
#endif
    return 0;
}

bool hasPPC(uint32_t physicalType, uint32_t formatIndex, uint32_t rotIndex) {
    if (ppc_table_map.find(PPC_IDX(physicalType, formatIndex, rotIndex)) !=
        ppc_table_map.end()) {
        return true;
    }
    return false;
}

int allocParcelData(int *fd, size_t size) {
    if (*fd >= 0)
        return NO_ERROR;
    else {
        int ionFd = exynos_ion_open();

        if (ionFd >= 0) {
            *fd = exynos_ion_alloc(ionFd, size, EXYNOS_ION_HEAP_SYSTEM_MASK, 0);
            if (*fd < 0) {
                ALOGE("Failed to ion alloc for parcel data");
                exynos_ion_close(ionFd);
                return -1;
            }
            exynos_ion_close(ionFd);
        } else {
            ALOGE("Failed to open ion client fd");
            return -1;
        }
    }
    return NO_ERROR;
}

compressionInfo_t getCompressionInfo(buffer_handle_t handle) {
    compressionInfo_t compressionInfo = {COMP_TYPE_NONE, 0, 0};

    if (handle) {
        if (isAFBCCompressed(handle))
            compressionInfo.type = COMP_TYPE_AFBC;
        else if (isSAJCCompressed(handle))
            compressionInfo.type = COMP_TYPE_SAJC;
        else
            compressionInfo.type = COMP_TYPE_NONE;

        if (isSAJCCompressed(handle)) {
            compressionInfo.SAJCMaxBlockSize = ExynosGraphicBufferMeta::get_sajc_independent_block_size(handle);
            compressionInfo.SAJCHeaderOffset = ExynosGraphicBufferMeta::get_sajc_key_offset(handle);
        }
    }

    return compressionInfo;
}

android_dataspace_t getRefinedDataspace(int halFormat, android_dataspace_t dataspace) {

    if (dataspace != HAL_DATASPACE_UNKNOWN) return dataspace;

    /* Google gave guide that HAL_DATSPACE_UNKNOWN should trigger
       the same behavior in HWC as HAL_DATASPACE_V0_SRGB. (11251035)
       So we modified data space from UNKNOWN to V0_SRGB */
    if (isFormatRgb(halFormat))
        return HAL_DATASPACE_V0_SRGB;
    else
        return HAL_DATASPACE_V0_BT709;

    return HAL_DATASPACE_V0_SRGB;

}
