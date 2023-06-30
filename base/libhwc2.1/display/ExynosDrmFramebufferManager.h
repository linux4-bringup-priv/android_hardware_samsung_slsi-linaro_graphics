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

#ifndef _EXYNOSDRMFRAMEBUFFERMANAGER_H
#define _EXYNOSDRMFRAMEBUFFERMANAGER_H
#include <sys/types.h>
#include <utils/Condition.h>
#include <utils/Mutex.h>
#include <list>
#include <array>
#include <thread>
#include <xf86drmMode.h>
#include <utils/Singleton.h>
#include "ExynosHWCTypes.h"
#include "ExynosDpuData.h"

constexpr uint32_t MAX_CACHED_BUFFERS = 32;  // TODO: find a good value for this
constexpr uint32_t MAX_CACHED_BUFFERS_NO_LAYER_NUM_CHANGE = MAX_CACHED_BUFFERS * 2;

/* Max plane number of buffer object */
#define HWC_DRM_BO_MAX_PLANES 4

using BufHandles = std::array<uint32_t, HWC_DRM_BO_MAX_PLANES>;

void freeBufHandle(int drmFd, uint32_t handle);
void removeFbs(int drmFd, std::list<uint32_t> &fbs);

class FramebufferManager : public Singleton<FramebufferManager> {
  public:
    FramebufferManager(){};
    ~FramebufferManager();
    void init(int drmFd);

    // get buffer for provided config, if a buffer with same config is already cached it will be
    // reused otherwise one will be allocated. returns fbId that can be used to attach to plane, any
    // buffers allocated/reused with this call will be staged, flip() call is expected after this
    // when frame is committed
    int32_t getBuffer(const uint32_t displayType, const exynos_win_config_data &config, uint32_t &fbId, const bool caching);

    // this should be called after frame update
    // this will move all staged buffers to front of the cached buffers queue
    // This will also schedule a cleanup of cached buffers if cached buffer list goes
    // beyond MAX_CACHED_BUFFERS_NO_LAYER_NUM_CHANGE
    void flip(uint32_t displayId, bool isActiveCommit);

    // release all currently tracked buffers
    void releaseAll();

    void updateLastActiveCommitTime(uint32_t displayType, nsecs_t time) {
        mLastActiveCommitTime[displayType] = time;
    };

    void onDisplayRemoved(const uint32_t displayId);

    void cleanupSignal(bool hasLayerNumChange = true) {
        bool needSignal = false;
        uint32_t maxCacheBuffer = hasLayerNumChange ? MAX_CACHED_BUFFERS : MAX_CACHED_BUFFERS_NO_LAYER_NUM_CHANGE;
        {
            Mutex::Autolock lock(mMutex);
            if (mCachedBuffers.size() > maxCacheBuffer)
                needSignal = true;
        }
        if (needSignal)
            mCondition.signal();
    };

    void removeBuffer(const uint64_t &bufferId);
    void removeBuffersForDisplay(const uint32_t displayType);
    void removeBuffersForOwner(const void *owner);

  private:
    uint32_t getBufHandleFromFd(int fd);
    // this struct should contain elements that can be used to identify framebuffer more easily
    struct Framebuffer {
        Framebuffer(int _drmFd, uint64_t _bufferId, uint32_t _displayType,
                    void *_owner, int _format, uint32_t _width, uint32_t _height,
                    uint64_t _modifier, uint32_t _fbId)
            : displayType(_displayType), owner(_owner), bufferId(_bufferId),
              format(_format), width(_width), height(_height), modifier(_modifier),
              fbId(_fbId), drmFd(_drmFd), removePending(false){};
        ~Framebuffer() {
            drmModeRmFB(drmFd, fbId);
        };
        void updateLastActiveTime(uint32_t displayType, nsecs_t time) {
            lastActiveTime[displayType] = time;
        };
        void pendRemove() { removePending = true; };

        uint32_t displayType;
        void *owner;
        /*
         * There could be framebuffer that has same bufferId
         * different format, size or modifier
         */
        uint64_t bufferId;
        uint32_t format;
        uint32_t width;
        uint32_t height;
        uint64_t modifier;

        uint32_t fbId;
        int drmFd;
        nsecs_t lastActiveTime[HWC_NUM_DISPLAY_TYPES] = {0};
        bool removePending;
    };
    using FBList = std::list<std::unique_ptr<Framebuffer>>;

    int addFB2WithModifiers(uint32_t width, uint32_t height, uint32_t pixel_format,
                            const BufHandles handles, const uint32_t pitches[4],
                            const uint32_t offsets[4], const uint64_t modifier[4], uint32_t *buf_id,
                            uint32_t flags);

    void removeFBsThreadRoutine();

    bool canRemoveBuffer(const std::unique_ptr<Framebuffer> &frameBuf); // REQUIRES(mMutex)
    void removeBufferInternal(std::function<bool(const std::unique_ptr<Framebuffer> &buf)> compareFunc);
    // Put the framebuffers at the back of the cached buffer queue that go beyond
    // MAX_CACHED_BUFFERS to the FBList. Framebuffers in the FBList would be
    // released by removeFBsThreadRoutine()
    void fillCleanupBuffer();

    // buffers that are going to be committed in the next atomic frame update
    FBList mStagingBuffers;
    // unused buffers that have been used recently, front of the queue has the most recently used
    // ones
    FBList mCachedBuffers;
    // buffers that are going to be removed
    FBList mCleanupBuffers;

    int mDrmFd = -1;
    nsecs_t mLastActiveCommitTime[HWC_NUM_DISPLAY_TYPES] = {0};

    std::thread mRmFBThread;
    bool mRmFBThreadRunning = false;
    Condition mCondition;
    Mutex mMutex;
};
#endif
