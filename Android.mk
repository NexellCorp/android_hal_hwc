#
# Copyright (C) 2008 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ifeq ($(strip $(BOARD_USES_NX_HWCOMPOSER)),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_PRELINK_MODULE := false

LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE := hwcomposer.$(TARGET_BOOTLOADER_BOARD_NAME)

LOCAL_SHARED_LIBRARIES := \
	liblog \
	libsync \
	libcutils \
	libhardware \
	libhardware_legacy \
	libutils \
	libbinder \
	libdrm

LOCAL_C_INCLUDES := \
	frameworks/native/include \
	system/core/include \
	hardware/libhardware/include \
	external/libdrm \
	external/libdrm/include/drm \
	system/core/include/utils \
	system/core/libsync \
	system/core/libsync/include \
	$(LOCAL_PATH)/../gralloc

LOCAL_CFLAGS := -DLOG_TAG=\"hwcomposer\"

LOCAL_SRC_FILES := \
	drmmode.cpp \
	drmproperty.cpp \
	drmconnector.cpp \
	drmcrtc.cpp \
	drmencoder.cpp \
	drmplane.cpp \
	drmresources.cpp \
	worker.cpp \
	vsyncworker.cpp \
	drmeventlistener.cpp \
	nexellimporter.cpp \
	hwcomposer.cpp

include $(BUILD_SHARED_LIBRARY)

endif #ifeq ($(strip $(BOARD_USES_NX_HWCOMPOSER)),true)
