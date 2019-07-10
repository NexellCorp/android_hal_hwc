/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_RENDER_WORKER_H_
#define ANDROID_RENDER_WORKER_H_

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <sync/sync.h>

#include <queue>
#include <mutex>

#include "worker.h"

namespace android {

class RenderWorker: public Worker {
public:
    RenderWorker():
        Worker("drm-rendere", HAL_PRIORITY_URGENT_DISPLAY) {
    }
    ~RenderWorker() override;

    int Init(int32_t id, void *ctx) {
        id_ = id;
        ctx_ = ctx;
        buffer_ = NULL;
        frame_count_ = 0;

        return InitWorker();
    }

    void QueueFB(buffer_handle_t buffer) {
        queue_.queue(buffer);
        /* HACK
         * SurfaceFlinger starts all display rendering by Primary VSync Event
         * So, if Secondary Display is slower than Primary, buffer is
         * accumulated. Below code is workaround for this situation.
         */
        if (id_ == 1 && queue_.size() >= 2) {
            queue_.pop();
            queue_fd_.pop(); /* nexell sync use */
        }
        Signal();
    }

    buffer_handle_t DequeueFB() {
        if (queue_.isEmpty())
            return NULL;
        return queue_.dequeue();
    }

    /* nexell sync use --------------------- */
    void QueueFD(int fd) {
        queue_fd_.queue(fd);
        //Signal();
    }

    int DequeueFD() {
        if (queue_fd_.isEmpty())
            return NULL;
        return queue_fd_.dequeue();
    }

    void FlushFD() {
        while (!queue_fd_.isEmpty())
            queue_fd_.dequeue();
    }
    /* nexell sync use --------------------- */

    void SetDisplayFrame(hwc_rect_t &d) {
        memcpy(&displayFrame_, &d, sizeof(d));
    }

protected:
    void Routine() override;

private:
    int Render(buffer_handle_t h);

    int32_t id_;
    void *ctx_;
    NXQueue<buffer_handle_t> queue_;
    /* nexell sync use */
    NXQueue<int> queue_fd_;
    hwc_rect_t displayFrame_;
    buffer_handle_t buffer_;
    unsigned frame_count_;
};

}

#endif
