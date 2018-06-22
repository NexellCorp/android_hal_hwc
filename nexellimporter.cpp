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

#include "drmresources.h"
#include "importer.h"

#include <hardware/gralloc.h>
#include <gralloc_priv.h>

#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cutils/log.h>
#include <hardware/gralloc.h>

namespace android {

class NexellImporter : public Importer {
public:
	NexellImporter(DrmResources *drm);
	~NexellImporter() override;

	int ImportBuffer(buffer_handle_t handle, hwc_drm_bo_t *bo) override;
	int ReleaseBuffer(hwc_drm_bo_t *bo) override;

private:
	uint32_t ConvertHalFormatToDrm(uint32_t hal_format);
	uint32_t GetBytePerPixel(uint32_t hal_format);

	DrmResources *drm_;
};

// static
Importer *Importer::CreateInstance(DrmResources *drm)
{
	Importer *importer = new NexellImporter(drm);
	if (!importer)
		return NULL;

	return importer;
}

NexellImporter::NexellImporter(DrmResources *drm)
	: drm_(drm)
{
}

NexellImporter::~NexellImporter()
{
}

uint32_t NexellImporter::ConvertHalFormatToDrm(uint32_t hal_format)
{
	switch (hal_format) {
	case HAL_PIXEL_FORMAT_RGB_888:
		return DRM_FORMAT_BGR888;
	case HAL_PIXEL_FORMAT_BGRA_8888:
		return DRM_FORMAT_ARGB8888;
	case HAL_PIXEL_FORMAT_RGBX_8888:
		return DRM_FORMAT_XBGR8888;
	case HAL_PIXEL_FORMAT_RGBA_8888:
		return DRM_FORMAT_ABGR8888;
	case HAL_PIXEL_FORMAT_RGB_565:
		return DRM_FORMAT_BGR565;
	case HAL_PIXEL_FORMAT_YV12:
		return DRM_FORMAT_YVU420;
	default:
		ALOGE("Cannot convert hal format to drm format %u", hal_format);
		return -EINVAL;
	}
}

uint32_t NexellImporter::GetBytePerPixel(uint32_t hal_format)
{
	switch (hal_format) {
	case HAL_PIXEL_FORMAT_RGB_888:
		return 3;
	case HAL_PIXEL_FORMAT_BGRA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_RGBA_8888:
		return 4;
	case HAL_PIXEL_FORMAT_RGB_565:
		return 2;
	case HAL_PIXEL_FORMAT_YV12:
		return 1;
	default:
		ALOGE("Cannot convert hal format to drm format %u", hal_format);
		return -EINVAL;
	}
}

int NexellImporter::ImportBuffer(buffer_handle_t handle, hwc_drm_bo_t *bo)
{
	private_handle_t *gr_handle = private_handle_t::dynamicCast(handle);
	if (!gr_handle) {
		ALOGE("%s: failed to dynamicCast to private_handle_t", __func__);
		return -EINVAL;
	}

	// for debugging
	// if (gr_handle->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
	// 	ALOGD("ImportBuffer for framebuffer: offset 0x%x", gr_handle->offset);

	uint32_t gem_handle;
	int ret = drmPrimeFDToHandle(drm_->fd(), gr_handle->share_fd, &gem_handle);
	if (ret) {
		ALOGE("%s: failed to import prime fd %d ret=%d", __func__,
			  gr_handle->share_fd, ret);
	}

	memset(bo, 0, sizeof(hwc_drm_bo_t));
	bo->width = gr_handle->width;
	bo->height = gr_handle->height;
	bo->format = ConvertHalFormatToDrm(gr_handle->format);
	bo->pitches[0] = gr_handle->stride * GetBytePerPixel(gr_handle->format);;
	bo->gem_handles[0] = gem_handle;
	bo->priv = (void *)handle;
	if (gr_handle->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER)
		bo->offsets[0] = gr_handle->offset;
	else
		bo->offsets[0] = 0;

	ret = drmModeAddFB2(drm_->fd(), bo->width, bo->height, bo->format,
						bo->gem_handles, bo->pitches, bo->offsets, &bo->fb_id,
						0);
	if (ret) {
		ALOGE("could not create drm fb %d", ret);
		return ret;
	}

	ALOGD("[nexellimporter] IMPORT %p", bo);
	return ret;
}

int NexellImporter::ReleaseBuffer(hwc_drm_bo_t *bo)
{
	if (bo->fb_id)
		if (drmModeRmFB(drm_->fd(), bo->fb_id))
			ALOGE("Failed to rm fb");

	struct drm_gem_close gem_close;
	memset(&gem_close, 0, sizeof(gem_close));
	int num_gem_handles = sizeof(bo->gem_handles) / sizeof(bo->gem_handles[0]);
	for (int i = 0; i < num_gem_handles; i++) {
		if (!bo->gem_handles[i])
			continue;

		gem_close.handle = bo->gem_handles[i];
		int ret = drmIoctl(drm_->fd(), DRM_IOCTL_GEM_CLOSE, &gem_close);
		if (ret)
			ALOGE("Failed to close gem handle %d %d", i, ret);
		else
			bo->gem_handles[i] = 0;
	}
	ALOGD("[nexellimporter] RELEASE %p", bo);
	return 0;
}

} // namespace android
