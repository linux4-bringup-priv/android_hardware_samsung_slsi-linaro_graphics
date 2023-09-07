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
#define ATRACE_TAG (ATRACE_TAG_GRAPHICS | ATRACE_TAG_HAL)
#include <xf86drm.h>
#include "exynos_drm_modifier.h"
#include "ExynosDrmFramebufferManager.h"
#include "ExynosHWCHelper.h"
#include "ExynosHWCDebug.h"

constexpr uint32_t MAX_PLANE_NUM = 3;
constexpr uint32_t SAJC_KEY_INDEX = 1;

uint64_t getSBWCModifierBits(const format_description &format_desc) {
    uint32_t sbwcType = format_desc.type & FORMAT_SBWC_MASK;
    if (sbwcType == 0) {
        if ((format_desc.halFormat == HAL_PIXEL_FORMAT_EXYNOS_420_SPN_SBWC_DECOMP) ||
            (format_desc.halFormat == HAL_PIXEL_FORMAT_EXYNOS_P010_N_SBWC_DECOMP))
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_NONE, 0);
        return 0;
    }
    if (sbwcType == SBWC_LOSSLESS)
        return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSLESS, 0);

    uint32_t bitType = format_desc.type & BIT_MASK;
    if (bitType == BIT10) {
        switch (sbwcType) {
        case SBWC_LOSSY_40:
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSY, SBWCL_10B_40);
        case SBWC_LOSSY_60:
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSY, SBWCL_10B_60);
        case SBWC_LOSSY_80:
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSY, SBWCL_10B_80);
        default:
            return 0;
        }
    } else if (bitType == BIT8) {
        switch (sbwcType) {
        case SBWC_LOSSY_50:
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSY, SBWCL_8B_50);
        case SBWC_LOSSY_75:
            return DRM_FORMAT_MOD_SAMSUNG_SBWC(SBWC_MOD_LOSSY, SBWCL_8B_75);
        default:
            return 0;
        }
    }

    return 0;
}

void freeBufHandle(int drmFd, uint32_t handle) {
    struct drm_gem_close gem_close {
        .handle = handle
    };
    int ret = drmIoctl(drmFd, DRM_IOCTL_GEM_CLOSE, &gem_close);
    if (ret) {
        ALOGE("Failed to close gem handle with error %d\n", ret);
    }
}

void removeFbs(int drmFd, std::list<uint32_t> &fbs) {
    for (auto &fb : fbs) {
        drmModeRmFB(drmFd, fb);
    }
}

ANDROID_SINGLETON_STATIC_INSTANCE(FramebufferManager);
FramebufferManager::~FramebufferManager() {
    {
        Mutex::Autolock lock(mMutex);
        mRmFBThreadRunning = false;
    }
    mCondition.signal();
    if (mRmFBThread.joinable()) {
        mRmFBThread.join();
    }

    releaseAll();
}

void FramebufferManager::init(int drmFd) {
    mDrmFd = drmFd;
    mRmFBThreadRunning = true;
    mRmFBThread = std::thread(&FramebufferManager::removeFBsThreadRoutine, this);
    pthread_setname_np(mRmFBThread.native_handle(), "RemoveFBsThread");
}

uint32_t FramebufferManager::getBufHandleFromFd(int fd) {
    uint32_t gem_handle = 0;

    int ret = drmPrimeFDToHandle(mDrmFd, fd, &gem_handle);
    if (ret) {
        HWC_LOGE_NODISP("drmPrimeFDToHandle failed with error %d", ret);
    }
    return gem_handle;
}

int FramebufferManager::addFB2WithModifiers(uint32_t width, uint32_t height, uint32_t pixel_format,
                                            const BufHandles handles, const uint32_t pitches[4],
                                            const uint32_t offsets[4], const uint64_t modifier[4],
                                            uint32_t *buf_id, uint32_t flags) {
    int ret = drmModeAddFB2WithModifiers(mDrmFd, width, height, pixel_format, handles.data(),
                                         pitches, offsets, modifier, buf_id, flags);
    if (ret)
        HWC_LOGE_NODISP("Failed to add fb error %d\n", ret);

    return ret;
}

bool FramebufferManager::canRemoveBuffer(const std::unique_ptr<Framebuffer> &frameBuf) {
    for (uint32_t i = 0; i < HWC_NUM_DISPLAY_TYPES; i++) {
        /* Can't remove framebuffer in active commit */
        if (mLastActiveCommitTime[i] &&
            mLastActiveCommitTime[i] == frameBuf->lastActiveTime[i]) {
            return false;
        }
    }
    return true;
}

void FramebufferManager::fillCleanupBuffer() {
    auto it = mCachedBuffers.end();
    it--;
    while (mCachedBuffers.size() > MAX_CACHED_BUFFERS) {
        bool stop = false;
        /*
         * MAX_CACHED_BUFFERS is more than 2,
         * it-- whouldn't be out of range
         */
        auto const cit = it;
        if (it == mCachedBuffers.begin())
            stop = true;
        else
            it--;
        if (canRemoveBuffer(*cit))
            mCleanupBuffers.splice(mCleanupBuffers.end(), mCachedBuffers, cit);

        if (stop)
            break;
    }
}

void FramebufferManager::removeFBsThreadRoutine() {
    FBList cleanupBuffers;
    while (true) {
        {
            Mutex::Autolock lock(mMutex);
            if (!mRmFBThreadRunning) {
                break;
            }
            mCondition.wait(mMutex);
            fillCleanupBuffer();
            cleanupBuffers.splice(cleanupBuffers.end(), mCleanupBuffers,
                                  mCleanupBuffers.begin(), mCleanupBuffers.end());
        }
        ATRACE_BEGIN("cleanup framebuffers");
        cleanupBuffers.clear();
        ATRACE_END();
    }
}

int32_t FramebufferManager::getBuffer(const uint32_t displayType,
                                      const exynos_win_config_data &config,
                                      uint32_t &fbId, const bool caching) {
    int ret = NO_ERROR;
    int drmFormat = DRM_FORMAT_UNDEFINED;
    uint32_t bpp = 0;
    uint32_t pitches[HWC_DRM_BO_MAX_PLANES] = {0};
    uint32_t offsets[HWC_DRM_BO_MAX_PLANES] = {0};
    uint64_t modifiers[HWC_DRM_BO_MAX_PLANES] = {0};
    uint32_t bufferNum, planeNum = 0;
    BufHandles handles = {0};
    uint32_t bufWidth, bufHeight = 0;

    funcReturnCallback retCallback([&]() {
        if (config.state != config.WIN_STATE_COLOR) {
            for (uint32_t i = 0; i < bufferNum; i++) {
                if (handles[i])
                    freeBufHandle(mDrmFd, handles[i]);
            }
        }
    });

    if (config.protection)
        modifiers[0] |= DRM_FORMAT_MOD_PROTECTION;

    if ((config.state == config.WIN_STATE_BUFFER) ||
        (config.state == config.WIN_STATE_CURSOR)) {
        bufWidth = config.src.f_w;
        bufHeight = config.src.f_h;

        auto formatDesc = config.format.getFormatDesc();
        if (formatDesc.halFormat == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            HWC_LOGE_NODISP("%s:: known hal format", __func__);
            return -EINVAL;
        }

        drmFormat = formatDesc.drmFormat;
        if (drmFormat == DRM_FORMAT_UNDEFINED) {
            HWC_LOGE_NODISP("%s:: unknown drm format (%s)",
                            __func__, formatDesc.name.c_str());
            return -EINVAL;
        }

        bpp = getBytePerPixelOfPrimaryPlane(formatDesc.halFormat);
        if ((bufferNum = formatDesc.bufferNum) == 0) {
            HWC_LOGE_NODISP("%s:: getBufferNumOfFormat(%s) error",
                            __func__, formatDesc.name.c_str());
            return -EINVAL;
        }
        if (((planeNum = formatDesc.planeNum) == 0) ||
            (planeNum > MAX_PLANE_NUM)) {
            HWC_LOGE_NODISP("%s:: getPlaneNumOfFormat(%s) error, planeNum(%d)",
                            __func__, formatDesc.name.c_str(), planeNum);
            return -EINVAL;
        }

        if (config.compressionInfo.type == COMP_TYPE_AFBC) {
            uint64_t compressed_modifier = AFBC_FORMAT_MOD_BLOCK_SIZE_16x16;
            switch (config.comp_src) {
            case DPP_COMP_SRC_G2D:
                compressed_modifier |= AFBC_FORMAT_MOD_SOURCE_G2D;
                break;
            case DPP_COMP_SRC_GPU:
                compressed_modifier |= AFBC_FORMAT_MOD_SOURCE_GPU;
                break;
            default:
                break;
            }
            modifiers[0] |= DRM_FORMAT_MOD_ARM_AFBC(compressed_modifier);
        } else if (config.compressionInfo.type == COMP_TYPE_SAJC) {
            uint32_t compressed_block_size = config.compressionInfo.SAJCMaxBlockSize;
            modifiers[0] |= DRM_FORMAT_MOD_SAMSUNG_SAJC(compressed_block_size);
            //SAJC buffer has 2 planes//
            planeNum++;
        } else {
            modifiers[0] |= getSBWCModifierBits(formatDesc);
        }

#if 1
        /* vOTF */
        if (config.vOtfEnable) {
            modifiers[0] |= DRM_FORMAT_MOD_SAMSUNG_VOTF(config.vOtfBufIndex);
        }
#else
        if (config.vOtfEnable) {
            HWC_LOGE_NODISP("%s %d", __func__, config.vOtfBufIndex);
        }
#endif

        for (uint32_t bufferIndex = 0; bufferIndex < bufferNum; bufferIndex++) {
            pitches[bufferIndex] = config.src.f_w * bpp;
            modifiers[bufferIndex] = modifiers[0];
        }

        if ((bufferNum == 1) && (planeNum > bufferNum)) {
            /* offset for cbcr */
            if (config.compressionInfo.type == COMP_TYPE_SAJC)
                offsets[SAJC_KEY_INDEX] = config.compressionInfo.SAJCHeaderOffset;
            for (uint32_t planeIndex = 1; planeIndex < planeNum; planeIndex++) {
                pitches[planeIndex] = pitches[0];
                modifiers[planeIndex] = modifiers[0];
            }
        }
    } else if (config.state == config.WIN_STATE_COLOR) {
        bufWidth = config.dst.w;
        bufHeight = config.dst.h;
        modifiers[0] |= DRM_FORMAT_MOD_SAMSUNG_COLORMAP;
        drmFormat = DRM_FORMAT_BGRA8888;
        handles[0] = 0xff000000;
        bpp = getBytePerPixelOfPrimaryPlane(HAL_PIXEL_FORMAT_BGRA_8888);
        pitches[0] = config.dst.w * bpp;
    } else {
        HWC_LOGE_NODISP("%s:: known config state(%d)",
                        __func__, config.state);
        return -EINVAL;
    }

    if (caching) {
        Mutex::Autolock lock(mMutex);
        auto it =
            std::find_if(mCachedBuffers.begin(), mCachedBuffers.end(),
                         [&config, &drmFormat, &bufWidth, &bufHeight, &modifiers](auto &buffer) { return (buffer->bufferId == config.buffer_id) &&
                                                                                                         (buffer->format == drmFormat) &&
                                                                                                         (buffer->width == bufWidth) &&
                                                                                                         (buffer->height == bufHeight) &&
                                                                                                         (buffer->modifier == modifiers[0]); });
        if (it != mCachedBuffers.end()) {
            fbId = (*it)->fbId;
            mStagingBuffers.splice(mStagingBuffers.end(), mCachedBuffers, it);
            return NO_ERROR;
        }
    }

    /* Get handles only if buffer is not in cache */
    if (config.state != config.WIN_STATE_COLOR) {
        for (uint32_t bufferIndex = 0; bufferIndex < bufferNum; bufferIndex++) {
            handles[bufferIndex] = getBufHandleFromFd(config.fd_idma[bufferIndex]);
            if (handles[bufferIndex] == 0) {
                return -EINVAL;
            }
        }
        if ((bufferNum == 1) && (planeNum > bufferNum)) {
            for (uint32_t planeIndex = 1; planeIndex < planeNum; planeIndex++) {
                handles[planeIndex] = handles[0];
            }
        }
    }

    ret = addFB2WithModifiers(bufWidth, bufHeight, drmFormat, handles, pitches, offsets, modifiers,
                              &fbId, modifiers[0] ? DRM_MODE_FB_MODIFIERS : 0);

    if (ret) {
        HWC_LOGE_NODISP(
            "%s:: Failed to add FB, fb_id(%d), ret(%d), f_w: %d, f_h: %d, dst.w: %d, dst.h: %d, "
            "format: %d %4.4s, buf_handles[%d, %d, %d, %d], "
            "pitches[%d, %d, %d, %d], offsets[%d, %d, %d, %d], modifiers[%#" PRIx64 ", %#" PRIx64
            ", %#" PRIx64 ", %#" PRIx64 "]",
            __func__, fbId, ret, config.src.f_w, config.src.f_h, config.dst.w, config.dst.h,
            drmFormat, (char *)&drmFormat, handles[0], handles[1], handles[2], handles[3],
            pitches[0], pitches[1], pitches[2], pitches[3], offsets[0], offsets[1], offsets[2],
            offsets[3], modifiers[0], modifiers[1], modifiers[2], modifiers[3]);
        return ret;
    }

    if (caching) {
        Mutex::Autolock lock(mMutex);
        mStagingBuffers.emplace_back(new Framebuffer(mDrmFd, config.buffer_id,
                                                     displayType, config.owner,
                                                     drmFormat, bufWidth, bufHeight,
                                                     modifiers[0], fbId));
    }

    return NO_ERROR;
}

void FramebufferManager::flip(uint32_t displayType, bool isActiveCommit) {
    {
        Mutex::Autolock lock(mMutex);
        nsecs_t time = systemTime(SYSTEM_TIME_MONOTONIC);
        if (isActiveCommit)
            updateLastActiveCommitTime(displayType, time);
        for (auto &it : mStagingBuffers) {
            if (isActiveCommit)
                it->updateLastActiveTime(displayType, time);
        }
        mCachedBuffers.splice(mCachedBuffers.begin(), mStagingBuffers);
    }
    cleanupSignal(false);
}

void FramebufferManager::releaseAll() {
    Mutex::Autolock lock(mMutex);
    mStagingBuffers.clear();
    mCachedBuffers.clear();
}

void FramebufferManager::onDisplayRemoved(const uint32_t displayType) {
    {
        /*
         * We don't need to check active commit time of removed display.
         * ExynosDisplay guarantees not to call present for the removed display
         * after this function call so active commit time wouldn't be
         * added after this function call.
         */
        Mutex::Autolock lock(mMutex);
        mLastActiveCommitTime[displayType] = 0;
    }
    removeBuffersForDisplay(displayType);
}

void FramebufferManager::removeBufferInternal(std::function<bool(const std::unique_ptr<Framebuffer> &buf)> compareFunc) {
    {
        Mutex::Autolock lock(mMutex);
        FBList::iterator it = mCachedBuffers.begin();
        while (it != mCachedBuffers.end()) {
            auto const cit = it;
            it++;
            if ((*cit)->removePending || compareFunc(*cit)) {
                if (canRemoveBuffer(*cit)) {
                    mCleanupBuffers.splice(mCleanupBuffers.end(), mCachedBuffers, cit);
                } else {
                    (*cit)->pendRemove();
                }
            }
        }
    }
    if (mCleanupBuffers.size() > 0)
        mCondition.signal();
}

void FramebufferManager::removeBuffer(const uint64_t &bufferId) {
    auto compareFunc = [=](const std::unique_ptr<Framebuffer> &buf) {
        return (buf->bufferId == bufferId);
    };
    removeBufferInternal(compareFunc);
}

void FramebufferManager::removeBuffersForDisplay(const uint32_t displayType) {
    auto compareFunc = [=](const std::unique_ptr<Framebuffer> &buf) {
        return (buf->displayType == displayType);
    };
    removeBufferInternal(compareFunc);
}

void FramebufferManager::removeBuffersForOwner(const void *owner) {
    auto compareFunc = [=](const std::unique_ptr<Framebuffer> &buf) {
        return (buf->owner == owner);
    };
    removeBufferInternal(compareFunc);
}
