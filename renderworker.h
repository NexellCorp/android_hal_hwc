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
#include <sw_sync.h>

#include <queue>
#include <mutex>

#include "worker.h"

namespace android {

template <class T>
class NXQueue
{
public:
	NXQueue() {
	};

	virtual ~NXQueue() {
	}

	void queue(const T& item) {
		std::lock_guard<std::mutex> guard(mutex);
		q.push(item);
	}

	const T& dequeue() {
		std::lock_guard<std::mutex> guard(mutex);
		const T& item = q.front();
		q.pop();
		return item;
	}

	void pop() {
		q.pop();
	}

	bool isEmpty() {
		std::lock_guard<std::mutex> guard(mutex);
		return q.empty();
	}

	size_t size() {
		std::lock_guard<std::mutex> guard(mutex);
		return q.size();
	}

	const T& getHead() {
		std::lock_guard<std::mutex> guard(mutex);
		return q.front();
	}

private:
	std::queue<T> q;
	std::mutex mutex;
};

class RenderWorker: public Worker {
public:
	RenderWorker():
		Worker("drm-rendere", HAL_PRIORITY_URGENT_DISPLAY) {
	}
	~RenderWorker() override;

	int Init(int32_t id, void *ctx) {
		id_ = id;
		ctx_ = ctx;
		sync_fence_fd_ = -1;
		next_sync_point_ = 1;
		sync_timeline_fd_ = sw_sync_timeline_create();
		buffer_ = NULL;
		frame_count_ = 0;
		stopping_ = false;

		return InitWorker();
	}

	void QueueFB(buffer_handle_t buffer) {
		queue_.queue(buffer);
		/* HACK
		 * SurfaceFlinger starts all display rendering by Primary VSync Event
		 * So, if Secondary Display is slower than Primary, buffer is
		 * accumulated. Below code is workaround for this situation.
		 */
		if (queue_.size() >= 2)
			queue_.pop();
		Signal();
	}

	buffer_handle_t DequeueFB() {
		if (queue_.isEmpty())
			return NULL;
		return queue_.dequeue();
	}

	void FlushFB() {
		while (!queue_.isEmpty())
			queue_.dequeue();
	}

	void StopRender() {
		Lock();
		stopping_ = true;
		Unlock();
		SignalLocked();
	}

	void SetDisplayFrame(hwc_rect_t &d) {
		memcpy(&displayFrame_, &d, sizeof(d));
	}

	int CreateSyncFence() {
		char str[256] = {0, };

		if (sync_fence_fd_ >= 0)
			close(sync_fence_fd_);

		sprintf(str, "render fence %d", next_sync_point_);
		sync_fence_fd_ = sw_sync_fence_create(sync_timeline_fd_, str,
											  next_sync_point_);
		return dup(sync_fence_fd_);
	}

	void ReleaseFence() {
		sw_sync_timeline_inc(sync_timeline_fd_, 1);
		next_sync_point_++;
	}

protected:
	void Routine() override;

private:
	int Render(buffer_handle_t h);

	int32_t id_;
	void *ctx_;
	NXQueue<buffer_handle_t> queue_;
	hwc_rect_t displayFrame_;
	unsigned next_sync_point_;
	int sync_timeline_fd_;
	int sync_fence_fd_;
	buffer_handle_t buffer_;
	unsigned frame_count_;
	bool stopping_;
};

}

#endif
