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

#ifndef ANDROID_WORKER_H_
#define ANDROID_WORKER_H_

#include <pthread.h>
#include <stdint.h>
#include <string>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <sync/sync.h>
#include <sw_sync.h>

#include <queue>
#include <mutex>

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

class Worker {
public:
    int Lock();
    int Unlock();

    // Must be called with the lock acquired
    int SignalLocked();
    int ExitLocked();

    // Convenience versions of above, acquires the lock
    int Signal();
    int Exit();

protected:
    Worker(const char *name, int priority);
    virtual ~Worker();

    int InitWorker();

    bool initialized() const;

    virtual void Routine() = 0;

    /*
     * Must be called with the lock acquired. max_nanoseconds may be negative to
     * indicate infinite timeout, otherwise it indicates the maximum time span to
     * wait for a signal before returning.
     * Returns -EINTR if interrupted by exit request, or -ETIMEDOUT if timed out
     */
    int WaitForSignalOrExitLocked(int64_t max_nanoseconds = -1);

private:
    static void *InternalRoutine(void *worker);

    // Must be called with the lock acquired
    int SignalThreadLocked(bool exit);

    std::string name_;
    int priority_;

    pthread_t thread_;
    pthread_mutex_t lock_;
    pthread_cond_t cond_;

    bool exit_;
    bool initialized_;
};

} // namespace android

#endif
