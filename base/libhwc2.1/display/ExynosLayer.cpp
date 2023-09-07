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
#include <hardware/hwcomposer_defs.h>
#include <hardware/exynos/ion.h>
#include <ui/GraphicBuffer.h>
#include "ExynosLayer.h"
#include "ExynosHWCDebug.h"
#include "VendorVideoAPI.h"
#include "ExynosGraphicBuffer.h"

using namespace android;
using vendor::graphics::ExynosGraphicBufferMeta;
constexpr unsigned int kHdrMultipliedVal = 50000;
constexpr unsigned int kHdrMultipliedLuminanceVal = 10000;

/**
 * ExynosLayer implementation
 */

ExynosLayer::ExynosLayer(DisplayInfo displayInfo)
    : ExynosMPPSource(MPP_SOURCE_LAYER, this),
      mDisplayInfo(displayInfo),
      mSfCompositionType(HWC2_COMPOSITION_INVALID),
      mCompositionType(HWC2_COMPOSITION_INVALID),
      mExynosCompositionType(HWC2_COMPOSITION_INVALID),
      mValidateCompositionType(HWC2_COMPOSITION_INVALID),
      mValidateExynosCompositionType(HWC2_COMPOSITION_INVALID),
      mOverlayInfo(0x0),
      mSupportedMPPFlag(0x0),
      mFps(0),
      mOverlayPriority(ePriorityLow),
      mGeometryChanged(0x0),
      mWindowIndex(-1),
      mCompressionInfo({COMP_TYPE_NONE, 0, 0}),
      mAcquireFence(-1),
      mPrevAcquireFence(-1),
      mReleaseFence(-1),
      mFrameCount(0),
      mLastFrameCount(0),
      mLastFpsTime(0),
      mLastLayerBuffer(NULL),
      mLayerBuffer(NULL),
      mDamageNum(0),
      mBlending(HWC2_BLEND_MODE_NONE),
      mPlaneAlpha(0),
      mTransform(0),
      mZOrder(1000),
      mDataSpace(HAL_DATASPACE_UNKNOWN),
      mLayerFlag(0x0),
      mIsHdrLayer(false),
      mIsHdr10PlusLayer(false),
      mIsHdrFrameworkPath(false),
      mIsHdr10PlusFrameworkPath(false),
      mMetaParcelFd(-1) {
    memset(&mDisplayFrame, 0, sizeof(mDisplayFrame));
    memset(&mSourceCrop, 0, sizeof(mSourceCrop));
    mVisibleRegionScreen.numRects = 0;
    mVisibleRegionScreen.rects = NULL;
    memset(&mColor, 0, sizeof(mColor));
    memset(&mPreprocessedInfo, 0, sizeof(mPreprocessedInfo));
    mCheckMPPFlag.clear();
    mCheckMPPFlag.reserve(MPP_LOGICAL_TYPE_NUM);
    mMetaParcel = NULL;
    mDamageRects.clear();
}

ExynosLayer::~ExynosLayer() {
    if (mM2mMPP != NULL) {
        for (int i = 0; i < NUM_MPP_SRC_BUFS; i++) {
            if (mM2mMPP->mPrevFrameInfo.srcInfo[i].bufferHandle == mLayerBuffer) {
                mM2mMPP->mPrevFrameInfo.srcInfo[i].bufferHandle = NULL;
            }
        }
    }

    if (mMetaParcel != NULL) {
        munmap(mMetaParcel, sizeof(ExynosVideoMeta));
        mMetaParcel = NULL;
    }

    if (mMetaParcelFd >= 0) {
        close(mMetaParcelFd);
        mMetaParcelFd = -1;
    }

    if (mAcquireFence != -1)
        mAcquireFence = mFenceTracer.fence_close(mAcquireFence, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_UNDEFINED,
                                                 "layer::destructor: mAcquireFence");

    if (mPrevAcquireFence != -1)
        mPrevAcquireFence = mFenceTracer.fence_close(mPrevAcquireFence, mDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_UNDEFINED,
                                                     "layer::destructor: mPrevAcquireFence");
}

void ExynosLayer::alignSourceCrop(DeviceValidateInfo &validateInfo) {
    mPreprocessedInfo.sourceCrop.left =
        pixel_align((int)mPreprocessedInfo.sourceCrop.left,
                    validateInfo.srcSizeRestriction.cropXAlign);
    mPreprocessedInfo.sourceCrop.top =
        pixel_align((int)mPreprocessedInfo.sourceCrop.top,
                    validateInfo.srcSizeRestriction.cropYAlign);
    mPreprocessedInfo.sourceCrop.right = mPreprocessedInfo.sourceCrop.left +
                                         pixel_align_down(WIDTH(mPreprocessedInfo.sourceCrop),
                                                          validateInfo.srcSizeRestriction.cropWidthAlign);
    mPreprocessedInfo.sourceCrop.bottom = mPreprocessedInfo.sourceCrop.top +
                                          pixel_align_down(HEIGHT(mPreprocessedInfo.sourceCrop),
                                                           validateInfo.srcSizeRestriction.cropHeightAlign);
}

void ExynosLayer::alignDisplayFrame(DeviceValidateInfo &validateInfo) {
    mPreprocessedInfo.displayFrame.right = mDisplayFrame.left +
                                           pixel_align(WIDTH(mDisplayFrame),
                                                       validateInfo.srcSizeRestriction.cropWidthAlign);
    mPreprocessedInfo.displayFrame.bottom = mDisplayFrame.top +
                                            pixel_align(HEIGHT(mDisplayFrame),
                                                        validateInfo.srcSizeRestriction.cropHeightAlign);

    if (mPreprocessedInfo.displayFrame.right > (int)(mDisplayInfo.xres)) {
        mPreprocessedInfo.displayFrame.left = mDisplayInfo.xres -
                                              pixel_align(WIDTH(mPreprocessedInfo.displayFrame),
                                                          validateInfo.srcSizeRestriction.cropWidthAlign);
        mPreprocessedInfo.displayFrame.right = mDisplayInfo.xres;
    }

    if (mPreprocessedInfo.displayFrame.bottom > (int)(mDisplayInfo.yres)) {
        mPreprocessedInfo.displayFrame.top = mDisplayInfo.yres -
                                             pixel_align_down(HEIGHT(mPreprocessedInfo.displayFrame),
                                                              validateInfo.srcSizeRestriction.cropHeightAlign);
        mPreprocessedInfo.displayFrame.bottom = mDisplayInfo.yres;
    }
}

void ExynosLayer::resizeDisplayFrame(DeviceValidateInfo &validateInfo) {
    uint32_t minDstWidth = validateInfo.dstSizeRestriction.minCropWidth;
    uint32_t minDstHeight = validateInfo.dstSizeRestriction.minCropHeight;

    if ((uint32_t)WIDTH(mDisplayFrame) < minDstWidth) {
        ALOGI("%s DRM layer displayFrame width %d is smaller than otf minWidth %d",
              mDisplayInfo.displayIdentifier.name.c_str(),
              WIDTH(mDisplayFrame), minDstWidth);
        mPreprocessedInfo.displayFrame.right = mDisplayFrame.left +
                                               pixel_align(WIDTH(mDisplayFrame), minDstWidth);

        if (mPreprocessedInfo.displayFrame.right > (int)(mDisplayInfo.xres)) {
            mPreprocessedInfo.displayFrame.left = mDisplayInfo.xres -
                                                  pixel_align(WIDTH(mPreprocessedInfo.displayFrame), minDstWidth);
            mPreprocessedInfo.displayFrame.right = mDisplayInfo.xres;
        }
    }
    if ((uint32_t)HEIGHT(mDisplayFrame) < minDstHeight) {
        ALOGI("%s DRM layer displayFrame height %d is smaller than vpp minHeight %d",
              mDisplayInfo.displayIdentifier.name.c_str(),
              HEIGHT(mDisplayFrame), minDstHeight);
        mPreprocessedInfo.displayFrame.bottom = mDisplayFrame.top +
                                                pixel_align(HEIGHT(mDisplayFrame), minDstHeight);

        if (mPreprocessedInfo.displayFrame.bottom > (int)(mDisplayInfo.yres)) {
            mPreprocessedInfo.displayFrame.top = mDisplayInfo.yres -
                                                 pixel_align(HEIGHT(mPreprocessedInfo.displayFrame), minDstHeight);
            mPreprocessedInfo.displayFrame.bottom = mDisplayInfo.yres;
        }
    }
}

int32_t ExynosLayer::doPreProcess(DeviceValidateInfo &validateInfo,
                                  uint64_t &outGeometryChanged) {
    overlay_priority priority = ePriorityLow;
    mIsHdrLayer = false;
    mLayerFlag = 0x0;

    mPreprocessedInfo.sourceCrop = mSourceCrop;
    mPreprocessedInfo.displayFrame = mDisplayFrame;
    mPreprocessedInfo.interlacedType = V4L2_FIELD_NONE;

    mLayerFlag = (mCompositionType == HWC2_COMPOSITION_SOLID_COLOR) ? (mLayerFlag | EXYNOS_HWC_DIM_LAYER) : (mLayerFlag & ~(EXYNOS_HWC_DIM_LAYER));

    funcReturnCallback retCallback([&]() {
        if (mOverlayPriority != priority)
            setGeometryChanged(GEOMETRY_LAYER_PRIORITY_CHANGED,
                               outGeometryChanged);
        mOverlayPriority = priority;
    });

    if (mLayerBuffer == nullptr)
        return NO_ERROR;

    /* Set HDR Flag */
    if (hasHdrInfo(mDataSpace))
        mIsHdrLayer = true;

    if (mLayerFormat.isYUV()) {
        mPreprocessedInfo.sourceCrop.top = (int)mSourceCrop.top;
        mPreprocessedInfo.sourceCrop.left = (int)mSourceCrop.left;
        mPreprocessedInfo.sourceCrop.bottom = (int)(mSourceCrop.bottom + 0.9);
        mPreprocessedInfo.sourceCrop.right = (int)(mSourceCrop.right + 0.9);

        uint64_t geometryChanged = 0;
        int ret = handleMetaData(geometryChanged);
        if ((ret == NO_ERROR) && geometryChanged)
            setGeometryChanged(geometryChanged, outGeometryChanged);

        /*
         * layer's sourceCrop should be aligned
         */
        alignSourceCrop(validateInfo);
    } else {
        mPreprocessedInfo.mUsePrivateFormat = false;
    }

    if ((getDrmMode(mLayerBuffer) != NO_DRM) || (mIsHdrLayer == true)) {
        /*
         * M2mMPP should be used for DRM, HDR video
         * layer's displayFrame is the source of DPP
         */
        if (mDisplayInfo.adjustDisplayFrame == true)
            alignDisplayFrame(validateInfo);

        if (getDrmMode(mLayerBuffer) != NO_DRM)
            resizeDisplayFrame(validateInfo);
    }

    if (getDrmMode(mLayerBuffer) != NO_DRM) {
        priority = ePriorityMax;
    } else if (is8KVideo(mLayerBuffer, mSourceCrop)) {
        priority = ePriorityMax;
    } else if (mLayerFormat.isYUV() || (mIsHdrLayer)) {
        priority = ePriorityHigh;
    } else if ((mDisplayInfo.cursorSupport == true) &&
               (mCompositionType == HWC2_COMPOSITION_CURSOR)) {
        priority = ePriorityMid;
    } else {
        priority = ePriorityLow;
    }

    return NO_ERROR;
}

int32_t ExynosLayer::setLayerBuffer(buffer_handle_t buffer, int32_t acquireFence,
                                    uint64_t &geometryFlag) {
    if (buffer != NULL) {
        if (ExynosGraphicBufferMeta::get_fd(buffer, 0) < 0)
            return HWC2_ERROR_BAD_LAYER;
    }

    int halFormat = ExynosGraphicBufferMeta::get_format(buffer);
    if ((mLayerBuffer == NULL) || (buffer == NULL))
        setGeometryChanged(GEOMETRY_LAYER_UNKNOWN_CHANGED, geometryFlag);
    else {
        if (getDrmMode(ExynosGraphicBufferMeta::get_producer_usage(mLayerBuffer)) != getDrmMode(ExynosGraphicBufferMeta::get_producer_usage(buffer)))
            setGeometryChanged(GEOMETRY_LAYER_DRM_CHANGED, geometryFlag);
        if (ExynosGraphicBufferMeta::get_format(mLayerBuffer) != halFormat)
            setGeometryChanged(GEOMETRY_LAYER_FORMAT_CHANGED, geometryFlag);
    }

    mPrevAcquireFence = mFenceTracer.fence_close(mPrevAcquireFence, mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_UNDEFINED,
                                                 "layer::setLayerBuffer: mPrevAcquireFence");
    mAcquireFence = mFenceTracer.fence_close(mAcquireFence, mDisplayInfo.displayIdentifier,
                                             FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_UNDEFINED,
                                             "layer::setLayerBuffer: mAcquireFence");

    mAcquireFence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                                 FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER, acquireFence);
    mFenceTracer.setFenceInfo(mAcquireFence, mDisplayInfo.displayIdentifier,
                              FENCE_TYPE_SRC_ACQUIRE, FENCE_IP_LAYER, FENCE_FROM);

    mPrevAcquireFence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_LAYER,
                                                     mFenceTracer.hwc_dup(mAcquireFence, mDisplayInfo.displayIdentifier,
                                                                          FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_LAYER, true));

    if (mReleaseFence >= 0)
        HWC_LOGE(mDisplayInfo.displayIdentifier, "Layer's release fence is not initialized");

    mReleaseFence = -1;
#ifdef DISABLE_FENCE
    if (mAcquireFence >= 0)
        mFenceTracer.fence_close(mAcquireFence);
    mAcquireFence = -1;
#endif

    uint32_t prevCompressionType = mCompressionInfo.type;

    mCompressionInfo = getCompressionInfo(buffer);

    if (mCompressionInfo.type != prevCompressionType)
        setGeometryChanged(GEOMETRY_LAYER_COMPRESSED_CHANGED, geometryFlag);

    if (buffer != NULL) {
        /*
         * HAL_DATASPACE_V0_JFIF = HAL_DATASPACE_STANDARD_BT601_625 |
         * HAL_DATASPACE_TRANSFER_SMPTE_170M | HAL_DATASPACE_RANGE_FULL,
         */
        if (ExynosGraphicBufferMeta::get_format(buffer) == HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL)
            setLayerDataspace(HAL_DATASPACE_V0_JFIF, geometryFlag);
    } else {
        setLayerDataspace(HAL_DATASPACE_UNKNOWN, geometryFlag);
    }

    HDEBUGLOGD(eDebugLayer, "layers bufferHandle: %p, mDataSpace: 0x%8x, acquireFence: %d, compressionType: %8x, format: 0x%" PRIx64 "",
               buffer, mDataSpace, mAcquireFence, mCompressionInfo.type, (uint64_t)ExynosGraphicBufferMeta::get_format(buffer));

    mLayerBuffer = buffer;
    mLayerFormat = ExynosFormat(halFormat, mCompressionInfo.type);

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerSurfaceDamage(hwc_region_t damage) {
    mDamageNum = damage.numRects;
    mDamageRects.clear();

    if (mDamageNum == 0)
        return HWC2_ERROR_NONE;

    for (size_t i = 0; i < mDamageNum; i++) {
        mDamageRects.push_back(damage.rects[i]);
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerBlendMode(int32_t /*hwc2_blend_mode_t*/ mode, uint64_t &geometryFlag) {
    //TODO mGeometryChanged  here
    if (mode < 0)
        return HWC2_ERROR_BAD_PARAMETER;
    if (mBlending != mode)
        setGeometryChanged(GEOMETRY_LAYER_BLEND_CHANGED, geometryFlag);
    mBlending = mode;
    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerColor(hwc_color_t color) {
    /* TODO : Implementation here */
    mColor = color;
    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerCompositionType(int32_t /*hwc2_composition_t*/ type, uint64_t &geometryFlag) {
    if (type < 0)
        return HWC2_ERROR_BAD_PARAMETER;

    if (type != mSfCompositionType)
        setGeometryChanged(GEOMETRY_LAYER_TYPE_CHANGED, geometryFlag);

    mSfCompositionType = type;
    mCompositionType = type;

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerDataspace(int32_t /*android_dataspace_t*/ dataspace,
                                       uint64_t &geometryFlag) {
    android_dataspace currentDataSpace = (android_dataspace_t)dataspace;
    if ((mLayerBuffer != NULL) && (ExynosGraphicBufferMeta::get_format(mLayerBuffer) == HAL_PIXEL_FORMAT_EXYNOS_YCrCb_420_SP_M_FULL))
        currentDataSpace = HAL_DATASPACE_V0_JFIF;
    else {
        /* Change legacy dataspace */
        switch (dataspace) {
        case HAL_DATASPACE_SRGB_LINEAR:
            currentDataSpace = HAL_DATASPACE_V0_SRGB_LINEAR;
            break;
        case HAL_DATASPACE_SRGB:
            currentDataSpace = HAL_DATASPACE_V0_SRGB;
            break;
        case HAL_DATASPACE_JFIF:
            currentDataSpace = HAL_DATASPACE_V0_JFIF;
            break;
        case HAL_DATASPACE_BT601_625:
            currentDataSpace = HAL_DATASPACE_V0_BT601_625;
            break;
        case HAL_DATASPACE_BT601_525:
            currentDataSpace = HAL_DATASPACE_V0_BT601_525;
            break;
        case HAL_DATASPACE_BT709:
            currentDataSpace = HAL_DATASPACE_V0_BT709;
            break;
        case HAL_DATASPACE_ARBITRARY:
            currentDataSpace = HAL_DATASPACE_UNKNOWN;
            break;
        default:
            currentDataSpace = (android_dataspace)dataspace;
            break;
        }
    }

    if (currentDataSpace != mDataSpace) {
        setGeometryChanged(GEOMETRY_LAYER_DATASPACE_CHANGED, geometryFlag);
    }
    mDataSpace = currentDataSpace;

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerDisplayFrame(hwc_rect_t frame, uint64_t &geometryFlag) {
    if ((frame.left != mDisplayFrame.left) ||
        (frame.top != mDisplayFrame.top) ||
        (frame.right != mDisplayFrame.right) ||
        (frame.bottom != mDisplayFrame.bottom))
        setGeometryChanged(GEOMETRY_LAYER_DISPLAYFRAME_CHANGED, geometryFlag);
    mDisplayFrame = frame;

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerPlaneAlpha(float alpha) {
    if (alpha < 0)
        return HWC2_ERROR_BAD_LAYER;

    mPlaneAlpha = alpha;

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerSourceCrop(hwc_frect_t crop, uint64_t &geometryFlag) {
    if ((crop.left != mSourceCrop.left) ||
        (crop.top != mSourceCrop.top) ||
        (crop.right != mSourceCrop.right) ||
        (crop.bottom != mSourceCrop.bottom)) {
        if ((mLayerBuffer && mLayerFormat.isYUV()) ||
            (WIDTH(crop) != WIDTH(mSourceCrop)) ||
            (HEIGHT(crop) != HEIGHT(mSourceCrop)))
            setGeometryChanged(GEOMETRY_LAYER_SOURCECROP_CHANGED, geometryFlag);

        mSourceCrop = crop;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerTransform(int32_t /*hwc_transform_t*/ transform,
                                       uint64_t &geometryFlag) {
    if (mTransform != transform) {
        setGeometryChanged(GEOMETRY_LAYER_TRANSFORM_CHANGED, geometryFlag);
        mTransform = transform;
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerVisibleRegion(hwc_region_t visible) {
    mVisibleRegionScreen = visible;

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerZOrder(uint32_t z, uint64_t &geometryFlag) {
    if (mZOrder != z) {
        setGeometryChanged(GEOMETRY_LAYER_ZORDER_CHANGED, geometryFlag);
        mZOrder = z;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerPerFrameMetadata(uint32_t numElements,
                                              const int32_t * /*hw2_per_frame_metadata_key_t*/ keys, const float *metadata) {
    mIsHdrFrameworkPath = false;

    if (allocMetaParcel() != NO_ERROR)
        return HWC_HAL_ERROR_INVAL;

    mIsHdrFrameworkPath = true;
    mMetaParcel->eType =
        static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_STATIC);
    for (uint32_t i = 0; i < numElements; i++) {
        HDEBUGLOGD(eDebugLayer, "HWC2: setLayerPerFrameMetadata key(%d), value(%7.5f)",
                   keys[i], metadata[i]);
        switch (keys[i]) {
        case HWC2_DISPLAY_RED_PRIMARY_X:
            mMetaParcel->sHdrStaticInfo.sType1.mR.x =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_DISPLAY_RED_PRIMARY_Y:
            mMetaParcel->sHdrStaticInfo.sType1.mR.y =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_DISPLAY_GREEN_PRIMARY_X:
            mMetaParcel->sHdrStaticInfo.sType1.mG.x =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_DISPLAY_GREEN_PRIMARY_Y:
            mMetaParcel->sHdrStaticInfo.sType1.mG.y =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_DISPLAY_BLUE_PRIMARY_X:
            mMetaParcel->sHdrStaticInfo.sType1.mB.x =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_DISPLAY_BLUE_PRIMARY_Y:
            mMetaParcel->sHdrStaticInfo.sType1.mB.y =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_WHITE_POINT_X:
            mMetaParcel->sHdrStaticInfo.sType1.mW.x =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_WHITE_POINT_Y:
            mMetaParcel->sHdrStaticInfo.sType1.mW.y =
                (unsigned int)(metadata[i] * kHdrMultipliedVal);
            break;
        case HWC2_MAX_LUMINANCE:
            mMetaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance =
                (unsigned int)(metadata[i] * kHdrMultipliedLuminanceVal);
            break;
        case HWC2_MIN_LUMINANCE:
            mMetaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance =
                (unsigned int)(metadata[i] * kHdrMultipliedLuminanceVal);
            break;
        case HWC2_MAX_CONTENT_LIGHT_LEVEL:
            /* Should be checked */
            mMetaParcel->sHdrStaticInfo.sType1.mMaxContentLightLevel =
                (unsigned int)(metadata[i]);
            break;
        case HWC2_MAX_FRAME_AVERAGE_LIGHT_LEVEL:
            /* Should be checked */
            mMetaParcel->sHdrStaticInfo.sType1.mMaxFrameAverageLightLevel =
                (unsigned int)(metadata[i]);
            break;
        default:
            return HWC2_ERROR_UNSUPPORTED;
        }
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerPerFrameMetadataBlobs(uint32_t numElements, const int32_t *keys, const uint32_t *sizes,
                                                   const uint8_t *metadata) {
    const uint8_t *metadata_start = metadata;

    mIsHdr10PlusFrameworkPath = false;
    mIsHdr10PlusLayer = false;

    for (uint32_t i = 0; i < numElements; i++) {
        HDEBUGLOGD(eDebugLayer, "HWC2: setLayerPerFrameMetadataBlobs key(%d)", keys[i]);
        switch (keys[i]) {
        case HWC2_HDR10_PLUS_SEI:
            if (allocMetaParcel() == NO_ERROR) {
                mMetaParcel->eType =
                    static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_DYNAMIC);
                ExynosHdrDynamicInfo *info = &(mMetaParcel->sHdrDynamicInfo);
                if (Exynos_parsing_user_data_registered_itu_t_t35(info, (void *)metadata_start) == NO_ERROR) {
                    mIsHdr10PlusFrameworkPath = true;
                    mIsHdr10PlusLayer = true;
                }
            } else {
                ALOGE("Layer has no metaParcel!");
                return HWC2_ERROR_UNSUPPORTED;
            }
            break;
        default:
            return HWC2_ERROR_BAD_PARAMETER;
        }
        metadata_start += sizes[i];
    }

    return HWC2_ERROR_NONE;
}

int32_t ExynosLayer::setLayerColorTransform(const float *matrix) {
    mLayerColorTransform.enable = false;
    for (uint32_t i = 0; i < TRANSFORM_MAT_SIZE; i++) {
        uint32_t height = i / TRANSFORM_MAT_WIDTH;
        uint32_t width = i % TRANSFORM_MAT_WIDTH;
        if (((width == height) && (matrix[i] != 1.0f)) ||
            ((width != height) && (matrix[i] != 0.0f)))
            mLayerColorTransform.enable = true;
        mLayerColorTransform.mat[i] = matrix[i];
    }

    return 0;
}

void ExynosLayer::resetValidateData() {
    mValidateCompositionType = HWC2_COMPOSITION_INVALID;
    mOtfMPP = NULL;
    mM2mMPP = NULL;
    mOverlayInfo = 0x0;
    mWindowIndex = -1;

    setSrcExynosImage(&mSrcImg);
    setDstExynosImage(&mDstImg);
}

int32_t ExynosLayer::setSrcExynosImage(exynos_image *src_img) {
    buffer_handle_t handle = mLayerBuffer;
    if (isDimLayer()) {
        src_img->exynosFormat = PredefinedFormat::exynosFormatRgba8;
        src_img->usageFlags = 0xb00;
        src_img->bufferHandle = 0;

        src_img->x = mPreprocessedInfo.displayFrame.left;
        src_img->y = mPreprocessedInfo.displayFrame.top;
        src_img->w = (mPreprocessedInfo.displayFrame.right - mPreprocessedInfo.displayFrame.left);
        src_img->h = (mPreprocessedInfo.displayFrame.bottom - mPreprocessedInfo.displayFrame.top);

        if (mDisplayInfo.displayIdentifier.id != UINT32_MAX) {
            src_img->fullWidth = mDisplayInfo.xres;
            src_img->fullHeight = mDisplayInfo.yres;
        } else {
            src_img->fullWidth = 1440;
            src_img->fullHeight = 2560;
        }

        src_img->layerFlags = mLayerFlag;
        src_img->acquireFenceFd = mAcquireFence;
        src_img->releaseFenceFd = -1;
        src_img->dataSpace = HAL_DATASPACE_V0_SRGB;
        src_img->blending = mBlending;
        src_img->transform = mTransform;
        src_img->compressionInfo = mCompressionInfo;
        src_img->planeAlpha = mPlaneAlpha;
        src_img->zOrder = mZOrder;
        src_img->color = mColor;

        return NO_ERROR;
    }

    if (handle == NULL) {
        src_img->fullWidth = 0;
        src_img->fullHeight = 0;
        src_img->exynosFormat = PredefinedFormat::exynosFormatUnDefined;
        src_img->usageFlags = 0x0;
        src_img->bufferHandle = handle;
    } else {
        ExynosGraphicBufferMeta gmeta(handle);

        if ((mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_TB) ||
            (mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_BT)) {
            src_img->fullWidth = (gmeta.stride * 2);
            src_img->fullHeight = pixel_align_down((gmeta.vstride / 2), 2);
        } else {
            src_img->fullWidth = gmeta.stride;
            src_img->fullHeight = gmeta.vstride;
        }
        if (!mPreprocessedInfo.mUsePrivateFormat)
            src_img->exynosFormat = mLayerFormat;
        else
            src_img->exynosFormat = mPreprocessedInfo.mPrivateFormat;
#ifdef GRALLOC_VERSION1
        src_img->usageFlags = gmeta.producer_usage;
#else
        src_img->usageFlags = (uint64_t)gmeta.flags;
#endif
        src_img->bufferHandle = handle;
    }
    src_img->x = (int)mPreprocessedInfo.sourceCrop.left;
    src_img->y = (int)mPreprocessedInfo.sourceCrop.top;
    src_img->w = (int)mPreprocessedInfo.sourceCrop.right - (int)mPreprocessedInfo.sourceCrop.left;
    src_img->h = (int)mPreprocessedInfo.sourceCrop.bottom - (int)mPreprocessedInfo.sourceCrop.top;
    if ((mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_TB) ||
        (mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_BT)) {
        while ((src_img->h % 2 != 0) ||
               (src_img->h > src_img->fullHeight)) {
            src_img->h -= 1;
        }
    }
    src_img->layerFlags = mLayerFlag;
    src_img->acquireFenceFd = mAcquireFence;
    src_img->releaseFenceFd = -1;

    src_img->dataSpace = mDataSpace;
    src_img->dataSpace = getRefinedDataspace(src_img->exynosFormat.halFormat(), src_img->dataSpace);

    src_img->blending = mBlending;
    src_img->transform = mTransform;
    src_img->compressionInfo = mCompressionInfo;
    src_img->planeAlpha = mPlaneAlpha;
    src_img->zOrder = mZOrder;
    src_img->color = mColor;
    /* Set HDR metadata */
    src_img->metaParcel = nullptr;
    src_img->metaType = VIDEO_INFO_TYPE_INVALID;
    if (mMetaParcel != nullptr) {
        src_img->metaParcel = mMetaParcel;
        src_img->metaType = mMetaParcel->eType;
    }

    src_img->needColorTransform = mLayerColorTransform.enable;

    if (src_img->needColorTransform)
        src_img->colorTransformMatrix = mLayerColorTransform.mat;
    else
        src_img->colorTransformMatrix = {
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    return NO_ERROR;
}

int32_t ExynosLayer::setDstExynosImage(exynos_image *dst_img) {
    buffer_handle_t handle = mLayerBuffer;

    if (handle == NULL) {
        dst_img->usageFlags = 0x0;
    } else {
#ifdef GRALLOC_VERSION1
        dst_img->usageFlags = ExynosGraphicBufferMeta::get_producer_usage(handle);
#else
        dst_img->usageFlags = (uint64_t)handle->flags;
#endif
    }

    if (isDimLayer()) {
        dst_img->usageFlags = 0xb00;
    }

    dst_img->exynosFormat = ExynosMPP::defaultMppDstFormat;
    dst_img->x = mPreprocessedInfo.displayFrame.left;
    dst_img->y = mPreprocessedInfo.displayFrame.top;
    dst_img->w = (mPreprocessedInfo.displayFrame.right - mPreprocessedInfo.displayFrame.left);
    dst_img->h = (mPreprocessedInfo.displayFrame.bottom - mPreprocessedInfo.displayFrame.top);
    dst_img->layerFlags = mLayerFlag;
    dst_img->acquireFenceFd = -1;
    dst_img->releaseFenceFd = -1;
    dst_img->bufferHandle = NULL;
    dst_img->dataSpace = HAL_DATASPACE_UNKNOWN;
    if (mDisplayInfo.displayIdentifier.id != UINT32_MAX) {
        dst_img->fullWidth = mDisplayInfo.xres;
        dst_img->fullHeight = mDisplayInfo.yres;
        if (mDisplayInfo.colorMode != HAL_COLOR_MODE_NATIVE) {
            dst_img->dataSpace = colorModeToDataspace(mDisplayInfo.colorMode);
        } else {
            if (hasHdrInfo(mDataSpace)) {
                android_dataspace hdrDataSpace =
                    (android_dataspace)(HAL_DATASPACE_STANDARD_DCI_P3 | HAL_DATASPACE_TRANSFER_GAMMA2_2 | HAL_DATASPACE_RANGE_LIMITED);
                if (mDisplayInfo.displayIdentifier.type == HWC_DISPLAY_EXTERNAL) {
                    if (mDisplayInfo.sinkHdrSupported == true)
                        dst_img->dataSpace = HAL_DATASPACE_UNKNOWN;
                    else
                        dst_img->dataSpace = hdrDataSpace;
                } else {
                    dst_img->dataSpace = hdrDataSpace;
                }
            }
        }
    } else {
        HWC_LOGE_NODISP("%s:: display info is not valid", __func__);
    }
    dst_img->blending = mBlending;
    dst_img->transform = mTransform;
    dst_img->compressionInfo.type = COMP_TYPE_NONE;
    dst_img->planeAlpha = mPlaneAlpha;
    dst_img->zOrder = mZOrder;
    dst_img->color = mColor;

    /* Set HDR metadata */
    dst_img->metaParcel = nullptr;
    dst_img->metaType = VIDEO_INFO_TYPE_INVALID;
    if (mMetaParcel != NULL) {
        dst_img->metaParcel = mMetaParcel;
        dst_img->metaType = mMetaParcel->eType;
    }

    return NO_ERROR;
}

int32_t ExynosLayer::resetAssignedResource() {
    int32_t ret = NO_ERROR;
    if (mM2mMPP != NULL) {
        HDEBUGLOGD(eDebugResourceManager, "\t\t %s mpp is reset", mM2mMPP->mName.c_str());
        mM2mMPP->resetAssignedState(this);
        mM2mMPP = NULL;
    }
    if (mOtfMPP != NULL) {
        HDEBUGLOGD(eDebugResourceManager, "\t\t %s mpp is reset", mOtfMPP->mName.c_str());
        mOtfMPP->resetAssignedState();
        mOtfMPP = NULL;
    }
    return ret;
}

void ExynosLayer::setSrcAcquireFence() {
    if (mAcquireFence == -1 && mPrevAcquireFence != -1) {
        mAcquireFence = mFenceTracer.checkFenceDebug(mDisplayInfo.displayIdentifier,
                                                     FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_LAYER,
                                                     mFenceTracer.hwc_dup(mPrevAcquireFence, mDisplayInfo.displayIdentifier,
                                                                          FENCE_TYPE_SRC_DUP_ACQUIRE, FENCE_IP_LAYER));
    }
}

void ExynosLayer::dump(String8 &result) {
    int32_t fd, fd1, fd2;
    if (mLayerBuffer != NULL) {
        ExynosGraphicBufferMeta gmeta(mLayerBuffer);
        fd = gmeta.fd;
        fd1 = gmeta.fd1;
        fd2 = gmeta.fd2;
    } else {
        fd = -1;
        fd1 = -1;
        fd2 = -1;
    }

    result.appendFormat("+---------------+------------+------+----------+------------+-----------+---------+------------+--------+------------------------+-----+----------+--------------------+\n");
    result.appendFormat("|     handle    |     fd     |  tr  | COMP_TYPE | dataSpace  |  format   |  blend  | planeAlpha | zOrder |          color         | fps | priority | windowIndex        | \n");
    result.appendFormat("+---------------+------------+------+----------+------------+-----------+---------+------------+--------+------------------------+-----+----------+--------------------+\n");
    result.appendFormat("|  %8p | %d, %d, %d | 0x%2x |   %8x  | 0x%8x | %s | 0x%4x  |    %1.3f    |    %d   | 0x%2x, 0x%2x, 0x%2x, 0x%2x |  %2d |    %2d    |    %d               |\n",
                        mLayerBuffer, fd, fd1, fd2, mTransform, mCompressionInfo.type, mDataSpace, mLayerFormat.name().c_str(),
                        mBlending, mPlaneAlpha, mZOrder, mColor.r, mColor.g, mColor.b, mColor.a, mFps, mOverlayPriority, mWindowIndex);
    result.appendFormat("|---------------+------------+------+------+------+-----+-----------+--------++-----+------+-----+--+-----------+------------++----+----------+--------------------+ \n");
    result.appendFormat("|    colorTr    |            sourceCrop           |          dispFrame       | type | exynosType | validateType | overlayInfo | supportedMPPFlag  | SRAM amount    |\n");
    result.appendFormat("|---------------+---------------------------------+--------------------------+------+------------+--------------+-------------+------------------------------------+ \n");
    result.appendFormat("|            %2d | %7.1f,%7.1f,%7.1f,%7.1f | %5d,%5d,%5d,%5d  |  %2d  |     %2d     |      %2d      |  0x%8x |    0x%8x       | %2d            |\n",
                        mLayerColorTransform.enable,
                        mPreprocessedInfo.sourceCrop.left, mPreprocessedInfo.sourceCrop.top, mPreprocessedInfo.sourceCrop.right, mPreprocessedInfo.sourceCrop.bottom,
                        mPreprocessedInfo.displayFrame.left, mPreprocessedInfo.displayFrame.top, mPreprocessedInfo.displayFrame.right, mPreprocessedInfo.displayFrame.bottom,
                        mCompositionType, mExynosCompositionType, mValidateCompositionType, mOverlayInfo, mSupportedMPPFlag, mHWResourceAmount[TDM_ATTR_SRAM_AMOUNT]);
    result.appendFormat("+---------------+---------------------------------+--------------------------+------+------------+--------------+-------------+------------------------------------+\n");

    if (mCheckMPPFlag.size()) {
        result.appendFormat("Unsupported MPP flags\n");
        for (auto it : mCheckMPPFlag) {
            result.appendFormat("[%s: 0x%" PRIx64 "] ",
                                getString(mppTypeStr, it.first),
                                it.second);
        }
        result.appendFormat("\n");
    }

    result.appendFormat("acquireFence: %d\n", mAcquireFence);
    if ((mOtfMPP == NULL) && (mM2mMPP == NULL))
        result.appendFormat("\tresource is not assigned.\n");
    if (mOtfMPP != NULL)
        result.appendFormat("\tassignedMPP: %s\n", mOtfMPP->mName.c_str());
    if (mM2mMPP != NULL)
        result.appendFormat("\tassignedM2mMPP: %s\n", mM2mMPP->mName.c_str());
    result.appendFormat("\tdump midImg\n");
    dumpExynosImage(result, mMidImg);
}

void ExynosLayer::printLayer() {
    int32_t fd, fd1, fd2;
    String8 result;
    if (mLayerBuffer != NULL) {
        ExynosGraphicBufferMeta gmeta(mLayerBuffer);
        fd = gmeta.fd;
        fd1 = gmeta.fd1;
        fd2 = gmeta.fd2;
    } else {
        fd = -1;
        fd1 = -1;
        fd2 = -1;
    }
    result.appendFormat("handle: %p [fd: %d, %d, %d], acquireFence: %d, mPrevAcquireFence: %d, tr: 0x%2x, COMP_TYPE: %8x, dataSpace: 0x%8x, format: %s\n",
                        mLayerBuffer, fd, fd1, fd2, mAcquireFence, mPrevAcquireFence, mTransform, mCompressionInfo.type, mDataSpace, mLayerFormat.name().c_str());
    result.appendFormat("\tblend: 0x%4x, planeAlpha: %3.1f, zOrder: %d, color[0x%2x, 0x%2x, 0x%2x, 0x%2x]\n",
                        mBlending, mPlaneAlpha, mZOrder, mColor.r, mColor.g, mColor.b, mColor.a);
    result.appendFormat("\tfps: %2d, priority: %d, windowIndex: %d, mLayerFlag: 0x%8x\n", mFps, mOverlayPriority, mWindowIndex, mLayerFlag);
    result.appendFormat("\tsourceCrop[%7.1f,%7.1f,%7.1f,%7.1f], dispFrame[%5d,%5d,%5d,%5d]\n",
                        mSourceCrop.left, mSourceCrop.top, mSourceCrop.right, mSourceCrop.bottom,
                        mDisplayFrame.left, mDisplayFrame.top, mDisplayFrame.right, mDisplayFrame.bottom);
    result.appendFormat("\ttype: %2d, exynosType: %2d, validateType: %2d\n",
                        mCompositionType, mExynosCompositionType, mValidateCompositionType);
    result.appendFormat("\toverlayInfo: 0x%8x, supportedMPPFlag: 0x%8x, geometryChanged: 0x%" PRIx64 "\n",
                        mOverlayInfo, mSupportedMPPFlag, mGeometryChanged);

    if (mCheckMPPFlag.size()) {
        result.appendFormat("Unsupported MPP flags\n");
        for (auto it : mCheckMPPFlag) {
            result.appendFormat("[%s: 0x%" PRIx64 "] ",
                                getString(mppTypeStr, it.first),
                                it.second);
        }
        result.appendFormat("\n");
    }

    ALOGD("%s", result.c_str());
    result.clear();

    if ((mOtfMPP == NULL) && (mM2mMPP == NULL))
        ALOGD("\tresource is not assigned.");
    if (mOtfMPP != NULL)
        ALOGD("\tassignedMPP: %s", mOtfMPP->mName.c_str());
    if (mM2mMPP != NULL)
        ALOGD("\tassignedM2mMPP: %s", mM2mMPP->mName.c_str());
    ALOGD("\t++ dump midImg ++");
    dumpExynosImage(result, mMidImg);
    ALOGD("%s", result.c_str());
}

void ExynosLayer::setGeometryChanged(uint64_t changedBit,
                                     uint64_t &outGeometryChanged) {
    mGeometryChanged |= changedBit;
    outGeometryChanged |= changedBit;
}

int ExynosLayer::allocMetaParcel() {
    /* Already allocated */
    if ((mMetaParcelFd >= 0) &&
        (mMetaParcel != NULL))
        return NO_ERROR;

    if (mMetaParcelFd < 0) {
        if (allocParcelData(&mMetaParcelFd, sizeof(ExynosVideoMeta)) != NO_ERROR) {
            ALOGE("%s:: Failed to alloc for metadata parcel", __func__);
            return -1;
        }
    }

    mMetaParcel =
        (ExynosVideoMeta *)mmap(0, sizeof(ExynosVideoMeta), PROT_READ | PROT_WRITE, MAP_SHARED, mMetaParcelFd, 0);
    if (mMetaParcel == NULL) {
        ALOGE("Failed to map metadata parcel");
        return -1;
    }

    return NO_ERROR;
}

void ExynosLayer::handleHdrStaticMetaData(ExynosVideoMeta *metaData) {
    std::optional<ui::Smpte2086> smpte2086;
    std::optional<ui::Cta861_3> cta861_3;
    auto &mapper = GraphicBufferMapper::get();

    if ((mapper.getSmpte2086(mLayerBuffer, &smpte2086) == OK) &&
        (mapper.getCta861_3(mLayerBuffer, &cta861_3) == OK) &&
        (smpte2086.has_value() || cta861_3.has_value()) &&
        (allocMetaParcel() == NO_ERROR)) {
        mIsHdrFrameworkPath = true;
        mMetaParcel->eType =
            static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_STATIC);

        if (smpte2086.has_value()) {
            const auto &metaValue = smpte2086.value();
            mMetaParcel->sHdrStaticInfo.sType1.mR.x =
                (unsigned int)(metaValue.primaryRed.x * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mR.y =
                (unsigned int)(metaValue.primaryRed.y * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mG.x =
                (unsigned int)(metaValue.primaryGreen.x * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mG.y =
                (unsigned int)(metaValue.primaryGreen.y * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mB.x =
                (unsigned int)(metaValue.primaryBlue.x * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mB.y =
                (unsigned int)(metaValue.primaryBlue.y * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mW.x =
                (unsigned int)(metaValue.whitePoint.x * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mW.y =
                (unsigned int)(metaValue.whitePoint.y * kHdrMultipliedVal);
            mMetaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance =
                (unsigned int)(metaValue.maxLuminance * kHdrMultipliedLuminanceVal);
            mMetaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance =
                (unsigned int)(metaValue.minLuminance * kHdrMultipliedLuminanceVal);

            HDEBUGLOGD(eDebugLayer, "HDR meta in buffer: r(%f, %f), g(%f, %f), b(%f, %f), wp(%f, %f), luminance(%f, %f)",
                       metaValue.primaryRed.x, metaValue.primaryRed.y,
                       metaValue.primaryGreen.x, metaValue.primaryGreen.y,
                       metaValue.primaryBlue.x, metaValue.primaryBlue.y,
                       metaValue.whitePoint.x, metaValue.whitePoint.y,
                       metaValue.maxLuminance, metaValue.minLuminance);
        }
        if (cta861_3.has_value()) {
            const auto &metaValue = cta861_3.value();
            /* Should be checked */
            mMetaParcel->sHdrStaticInfo.sType1.mMaxContentLightLevel =
                (unsigned int)(metaValue.maxContentLightLevel);
            mMetaParcel->sHdrStaticInfo.sType1.mMaxFrameAverageLightLevel =
                (unsigned int)(metaValue.maxFrameAverageLightLevel);
            HDEBUGLOGD(eDebugLayer, "HDR meta in buffer: max content light(%f), max average light(%f)",
                       metaValue.maxContentLightLevel,
                       metaValue.maxFrameAverageLightLevel);
        }
    } else if (!mIsHdrFrameworkPath &&
               (metaData->eType & VIDEO_INFO_TYPE_HDR_STATIC) &&
               (allocMetaParcel() == NO_ERROR)) {
        mMetaParcel->eType =
            static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_STATIC);
        mMetaParcel->sHdrStaticInfo = metaData->sHdrStaticInfo;
        HDEBUGLOGD(eDebugLayer, "HWC2: Static metadata min(%d), max(%d)",
                   mMetaParcel->sHdrStaticInfo.sType1.mMinDisplayLuminance,
                   mMetaParcel->sHdrStaticInfo.sType1.mMaxDisplayLuminance);
    }
}

void ExynosLayer::handleHdrDynamicMetaData(ExynosVideoMeta *metaData,
                                           uint64_t &geometryChanged) {
    std::optional<std::vector<uint8_t>> smpte2094_40;
    auto &mapper = GraphicBufferMapper::get();

    if ((mapper.getSmpte2094_40(mLayerBuffer, &smpte2094_40) == OK) &&
        smpte2094_40.has_value() &&
        (allocMetaParcel() == NO_ERROR)) {
        mMetaParcel->eType =
            static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_DYNAMIC);

        const auto &metaValue = smpte2094_40.value();
        ExynosHdrDynamicInfo *info = &(mMetaParcel->sHdrDynamicInfo);
        int32_t prevFlag = mLayerFlag & EXYNOS_HWC_FORCE_CLIENT_HDR_META_ERROR;
        if (Exynos_parsing_user_data_registered_itu_t_t35(info, (void *)metaValue.data())) {
            mLayerFlag |= EXYNOS_HWC_FORCE_CLIENT_HDR_META_ERROR;
            mIsHdr10PlusFrameworkPath = false;
            mIsHdr10PlusLayer = false;
        } else {
            mLayerFlag &= ~(EXYNOS_HWC_FORCE_CLIENT_HDR_META_ERROR);
            mIsHdr10PlusFrameworkPath = true;
            mIsHdr10PlusLayer = true;
            HDEBUGLOGD(eDebugLayer, "HDR dynamic meta in buffer: metadata size(%zu)",
                       metaValue.size());
        }
        if (prevFlag != (mLayerFlag & EXYNOS_HWC_FORCE_CLIENT_HDR_META_ERROR))
            geometryChanged |= GEOMETRY_LAYER_HDR_META_CHANGED;
    } else if (!mIsHdr10PlusFrameworkPath &&
               (metaData->eType & VIDEO_INFO_TYPE_HDR_DYNAMIC) &&
               (allocMetaParcel() == NO_ERROR)) {
        /* Reserved field for dynamic meta data */
        /* Currently It's not be used not only HWC but also OMX */
        mMetaParcel->eType =
            static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_DYNAMIC);
        mMetaParcel->sHdrDynamicInfo = metaData->sHdrDynamicInfo;
        mIsHdr10PlusLayer = true;
        HDEBUGLOGD(eDebugLayer, "HWC2: Layer has dynamic metadata");
    }
}
int32_t ExynosLayer::handleMetaData(uint64_t &outGeometryChanged) {
    ExynosGraphicBufferMeta gmeta(mLayerBuffer);
    outGeometryChanged = 0;

    ExynosVideoMeta *metaData =
        (ExynosVideoMeta *)ExynosGraphicBufferMeta::get_video_metadata(
            mLayerBuffer);

    if (metaData == MAP_FAILED) {
        HWC_LOGE(mDisplayInfo.displayIdentifier,
                 "Layer's metadata map failed!!");
        return -EFAULT;
    } else if (metaData == nullptr) {
        return NO_ERROR;
    }

    handleHdrStaticMetaData(metaData);
    handleHdrDynamicMetaData(metaData, outGeometryChanged);

    if (metaData->eType & VIDEO_INFO_TYPE_INTERLACED) {
        mPreprocessedInfo.interlacedType = metaData->data.dec.nInterlacedType;
        if ((mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_BT) &&
            ((int)mSourceCrop.left < (int)(gmeta.stride))) {
            mPreprocessedInfo.sourceCrop.left = (int)mSourceCrop.left +
                                                gmeta.stride;
            mPreprocessedInfo.sourceCrop.right = (int)mSourceCrop.right +
                                                 gmeta.stride;
        }
        if (mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_TB ||
            mPreprocessedInfo.interlacedType == V4L2_FIELD_INTERLACED_BT) {
            mPreprocessedInfo.sourceCrop.top = (int)(mSourceCrop.top) / 2;
            mPreprocessedInfo.sourceCrop.bottom = (int)(mSourceCrop.bottom) / 2;
        }
    }

    bool prev = mPreprocessedInfo.mUsePrivateFormat;
    if (isFormatYUV(gmeta.format) && (metaData->eType & VIDEO_INFO_TYPE_CHECK_PIXEL_FORMAT)) {
        mPreprocessedInfo.mUsePrivateFormat = true;
        mPreprocessedInfo.mPrivateFormat = metaData->nPixelFormat;
    } else {
        mPreprocessedInfo.mUsePrivateFormat = false;
        mPreprocessedInfo.mPrivateFormat = mLayerFormat;
    }
    if (prev != mPreprocessedInfo.mUsePrivateFormat)
        outGeometryChanged |= GEOMETRY_LAYER_FORMAT_CHANGED;

    return NO_ERROR;
}

bool ExynosLayer::isDimLayer() {
    if (mLayerFlag & EXYNOS_HWC_DIM_LAYER)
        return true;
    return false;
}

int ExynosLayer::clearHdrDynamicType() {
    if ((mMetaParcel != NULL) && (mMetaParcel->eType & VIDEO_INFO_TYPE_HDR_DYNAMIC)) {
        mMetaParcel->eType = static_cast<ExynosVideoInfoType>(mMetaParcel->eType & ~VIDEO_INFO_TYPE_HDR_DYNAMIC);
        HDEBUGLOGD(eDebugLayer, "%s: hdr dynamic info type is clear", __func__);
        return NO_ERROR;
    }
    return -1;
}

int ExynosLayer::restoreHdrDynamicType() {
    if ((mMetaParcel != NULL) && (mIsHdr10PlusLayer && !(mMetaParcel->eType & VIDEO_INFO_TYPE_HDR_DYNAMIC))) {
        mMetaParcel->eType = static_cast<ExynosVideoInfoType>(mMetaParcel->eType | VIDEO_INFO_TYPE_HDR_DYNAMIC);
        HDEBUGLOGD(eDebugLayer, "%s: hdr dynamic info type is restored", __func__);
        return NO_ERROR;
    }
    return -1;
}
