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

#define LOG_TAG "hwc-drm-resources"

#include "drmresources.h"
#include "autofd.h"

#include <cinttypes>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cutils/log.h>

namespace android {

DrmResources::DrmResources()
	: event_listener_(this)
{
}

DrmResources::~DrmResources()
{
	event_listener_.Exit();
}

int DrmResources::Init()
{
	const char *path = "/dev/dri/card0";

	fd_.Set(open(path, O_RDWR));
	if (fd() < 0) {
		ALOGE("Failed to open dri device: ret(%s)", strerror(-errno));
		return -ENODEV;
	}

	// want to get all plane list including primary
	int ret = drmSetClientCap(fd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		ALOGE("Failed to set universal plane cap: ret(%d)", ret);
		return ret;
	}

	ret = drmSetClientCap(fd(), DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		ALOGE("Failed to set atomic cap: ret(%d)", ret);
		return ret;
	}

	drmModeResPtr res = drmModeGetResources(fd());
	if (!res) {
		ALOGE("Failed to drmModeGetResources()");
		return -ENODEV;
	}

	bool found_primary = false;
	int display_num = 1;

	for (int i = 0; !ret && i < res->count_crtcs; ++i) {
		drmModeCrtcPtr c = drmModeGetCrtc(fd(), res->crtcs[i]);
		if (!c) {
			ALOGE("Failed to drmModeGetCrtc for %d", res->crtcs[i]);
			ret = -ENODEV;
			break;
		}

		std::unique_ptr<DrmCrtc> crtc(new DrmCrtc(this, c, i));
		drmModeFreeCrtc(c);

		ret = crtc->Init();
		if (ret) {
			ALOGE("Failed to initialize crtc %d", res->crtcs[i]);
			break;
		}
		crtcs_.emplace_back(std::move(crtc));
	}

	for (int i = 0; !ret && i < res->count_encoders; ++i) {
		drmModeEncoderPtr e = drmModeGetEncoder(fd(), res->encoders[i]);
		if (!e) {
			ALOGE("Failed to drmModeGetEncoder for %d", res->encoders[i]);
			ret = -EINVAL;
			break;
		}

		std::vector<DrmCrtc *> possible_crtcs;
		DrmCrtc *current_crtc = NULL;
		for (auto &crtc: crtcs_) {
			if ((1 << crtc->pipe()) & e->possible_crtcs)
				possible_crtcs.push_back(crtc.get());

			if (crtc->id() == e->crtc_id)
				current_crtc = crtc.get();
		}

		std::unique_ptr<DrmEncoder> enc(new DrmEncoder(e,
													   current_crtc,
													   possible_crtcs));
		drmModeFreeEncoder(e);
		encoders_.emplace_back(std::move(enc));
	}

	for (int i = 0; !ret && i < res->count_connectors; ++i) {
		drmModeConnectorPtr c = drmModeGetConnector(fd(), res->connectors[i]);
		if (!c) {
			ALOGE("Failed to drmModeGetConnectors for %d", res->connectors[i]);
			ret = -ENODEV;
			break;
		}

		std::vector<DrmEncoder *> possible_encoders;
		DrmEncoder *current_encoder = NULL;
		for (int j = 0; j < c->count_encoders; ++j) {
			for (auto &encoder : encoders_) {
				if (encoder->id() == c->encoders[j])
					possible_encoders.push_back(encoder.get());
				if (encoder->id() == c->encoder_id)
					current_encoder = encoder.get();
			}
		}

		std::unique_ptr<DrmConnector> conn(new DrmConnector(this, c,
															current_encoder,
															possible_encoders));
		drmModeFreeConnector(c);

		ret = conn->Init();
		if (ret) {
			ALOGE("Init connector %d failed", res->connectors[i]);
			break;
		}

		if (!found_primary) {
			conn->set_display(0);
			found_primary = true;
		} else {
			conn->set_display(display_num);
			++display_num;
		}

		connectors_.emplace_back(std::move(conn));
	}

	drmModeFreeResources(res);
	if (ret)
		return ret;

	drmModePlaneResPtr plane_res = drmModeGetPlaneResources(fd());
	if (!plane_res) {
		ALOGE("Failed to drmModeGetPlaneResources");
		return -ENOENT;
	}

	for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
		drmModePlanePtr p = drmModeGetPlane(fd(), plane_res->planes[i]);
		if (!p) {
			ALOGE("Failed to drmModeGetPlane for %d", plane_res->planes[i]);
			ret = -ENODEV;
			break;
		}

		std::unique_ptr<DrmPlane> plane(new DrmPlane(this, p));

		drmModeFreePlane(p);

		ret = plane->Init();
		if (ret) {
			ALOGE("Init plane %d failed", plane_res->planes[i]);
			break;
		}

		planes_.emplace_back(std::move(plane));
	}

	drmModeFreePlaneResources(plane_res);
	if (ret)
		return ret;

	ret = event_listener_.Init();
	if (ret) {
		ALOGE("Can't initialize event listener %d", ret);
		return ret;
	}

	for (auto &conn: connectors_) {
		ret = CreateDisplayPipe(conn.get());
		if (ret) {
			ALOGE("Failed to CreateDisplayPipe %d with %d", conn->id(), ret);
			return ret;
		}
	}

	return 0;
}

DrmConnector *DrmResources::GetConnectorForDisplay(int display) const
{
	for (auto &conn: connectors_)
		if (conn->display() == display)
			return conn.get();

	return NULL;
}

DrmCrtc *DrmResources::GetCrtcForDisplay(int display) const
{
	for (auto &crtc: crtcs_)
		if (crtc->display() == display)
			return crtc.get();

	return NULL;
}

DrmPlane *DrmResources::GetPlane(uint32_t id) const
{
	for (auto &plane: planes_)
		if (plane->id() == id)
			return plane.get();

	return NULL;
}

uint32_t DrmResources::next_mode_id()
{
	return ++mode_id_;
}

int DrmResources::TryEncoderForDisplay(int display, DrmEncoder *enc)
{
	DrmCrtc *crtc = enc->crtc();
	if (crtc && crtc->can_bind(display)) {
		crtc->set_display(display);
		return 0;
	}

	for (DrmCrtc *crtc : enc->possible_crtcs()) {
		if (crtc == enc->crtc())
			continue;

		if (crtc->can_bind(display)) {
			enc->set_crtc(crtc);
			crtc->set_display(display);
			return 0;
		}
	}

	return -EAGAIN;
}

int DrmResources::CreateDisplayPipe(DrmConnector *connector)
{
	int display = connector->display();

	if (connector->encoder()) {
		int ret = TryEncoderForDisplay(display, connector->encoder());
		if (!ret) {
			return 0;
		} else if (ret != -EAGAIN) {
			ALOGE("Could not set mode %d/%d", display, ret);
			return ret;
		}
	}

	for (DrmEncoder *enc : connector->possible_encoders()) {
		int ret = TryEncoderForDisplay(display, enc);
		if (!ret) {
			connector->set_encoder(enc);
			return 0;
		} else if (ret != -EAGAIN) {
			ALOGE("Could not set mode %d/%d", display, ret);
			return ret;
		}
	}

	ALOGE("Could not find a suitable encoder/crtc for display %d",
		  connector->display());
	return -ENODEV;
}

int DrmResources::CreatePropertyBlob(void *data, size_t length,
									 uint32_t *blob_id)
{
	struct drm_mode_create_blob create_blob;

	memset(&create_blob, 0, sizeof(create_blob));
	create_blob.length = length;
	create_blob.data = (__u64)data;

	int ret = drmIoctl(fd(), DRM_IOCTL_MODE_CREATEPROPBLOB, &create_blob);
	if (ret) {
		ALOGE("Failed to create mode property blob %d", ret);
		return ret;
	}

	*blob_id = create_blob.blob_id;
	return 0;
}

int DrmResources::DestroyPropertyBlob(uint32_t blob_id)
{
	if (!blob_id)
		return 0;

	struct drm_mode_destroy_blob destroy_blob;
	memset(&destroy_blob, 0, sizeof(destroy_blob));
	destroy_blob.blob_id = (__u32)blob_id;

	int ret = drmIoctl(fd(), DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy_blob);
	if (ret) {
		ALOGE("Failed to destroy mode property blob %" PRIu32 "/%d", blob_id,
			  ret);
		return ret;
	}

	return 0;
}

int DrmResources::SetDisplayActiveMode(int display, const DrmMode &mode)
{
	DrmConnector *connector = GetConnectorForDisplay(display);
	if (!connector) {
		ALOGE("Could not locate connector for display %d", display);
		return -ENODEV;
	}

	DrmCrtc *crtc = GetCrtcForDisplay(display);
	if (!crtc) {
		ALOGE("Could not locate crtc for display %d", display);
		return -ENODEV;
	}

	drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
	if (!pset) {
		ALOGE("Failed to allocate property set");
		return -ENOMEM;
	}

	struct drm_mode_modeinfo drm_mode;
	memset(&drm_mode, 0, sizeof(drm_mode));
	mode.ToDrmModeModeInfo(&drm_mode);

	uint32_t id = 0;
	int ret = CreatePropertyBlob(&drm_mode,
								 sizeof(struct drm_mode_modeinfo), &id);
	// TODO: DestroyPropertyBlob(id);
	if (ret) {
		ALOGE("Failed to create mode property blob %d", ret);
		return ret;
	}

	ret = drmModeAtomicAddProperty(pset, crtc->id(),
								   crtc->mode_property().id(),
								   id) < 0 ||
		drmModeAtomicAddProperty(pset, connector->id(),
								 connector->crtc_id_property().id(),
								 crtc->id()) < 0;
	if (ret) {
		ALOGE("Failed to add blob %d to pset", id);
		drmModeAtomicFree(pset);
		return ret;
	}

#if 0
	DrmPlane *primary_plane = NULL;
	for (auto &plane: planes_)
		if (plane->type() == DRM_PLANE_TYPE_PRIMARY &&
			plane->GetCrtcSupported(*crtc))
			primary_plane = plane.get();

	if (!primary_plane) {
		ALOGE("FATAL_ERROR: can't get primary plane for display %d", display);
		return -ENOENT;
	}

	ret = drmModeAtomicAddProperty(pset, primary_plane->id(),
								   primary_plane->crtc_property().id(),
								   crtc->id());
	if (ret) {
		ALOGE("Failed to add plane property\n");
		return ret;
	}
#endif

	uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd(), pset, flags, this);
	if (ret) {
		ALOGE("Failed to commit pset ret=%d\n", ret);
		drmModeAtomicFree(pset);
		return ret;
	}

	if (pset)
		drmModeAtomicFree(pset);

	connector->set_active_mode(mode);
	return 0;
}

int DrmResources::SetDpmsMode(int display, uint64_t mode)
{
	DrmConnector *conn = GetConnectorForDisplay(display);
	if (!conn) {
		ALOGE("Could not locate connector for display %d", display);
		return -ENODEV;
	}

	const DrmProperty &prop = conn->dpms_property();
	int ret = drmModeConnectorSetProperty(fd(), conn->id(), prop.id(), mode);
	ALOGD("Display %d SetDpmsMode %lld", display, mode);
	if (ret) {
		ALOGE("Failed to set DPMS property for connector %d", conn->id());
		return ret;
	}

	return 0;
}

DrmEventListener *DrmResources::event_listener()
{
	return &event_listener_;
}

int DrmResources::GetProperty(uint32_t obj_id, uint32_t obj_type,
							  const char *prop_name, DrmProperty *property)
{
	drmModeObjectPropertiesPtr props;

	props = drmModeObjectGetProperties(fd(), obj_id, obj_type);
	if (!props) {
		ALOGE("Failed to get properties for %d/%x", obj_id, obj_type);
		return -ENODEV;
	}

	bool found = false;
	for (int i = 0; !found && (size_t)i < props->count_props; ++i) {
		drmModePropertyPtr p = drmModeGetProperty(fd(), props->props[i]);

		if (!strcmp(p->name, prop_name)) {
			property->Init(p, props->prop_values[i]);
			found = true;
		}
		drmModeFreeProperty(p);
	}

	drmModeFreeObjectProperties(props);
	return found ? 0 : -ENOENT;
}

int DrmResources::GetPlaneProperty(const DrmPlane &plane, const char *prop_name,
								   DrmProperty *property)
{
	return GetProperty(plane.id(), DRM_MODE_OBJECT_PLANE, prop_name, property);
}

int DrmResources::GetCrtcProperty(const DrmCrtc &crtc, const char *prop_name,
								  DrmProperty *property)
{
	return GetProperty(crtc.id(), DRM_MODE_OBJECT_CRTC, prop_name, property);
}

int DrmResources::GetConnectorProperty(const DrmConnector &connector,
									   const char *prop_name,
									   DrmProperty *property)
{
	return GetProperty(connector.id(), DRM_MODE_OBJECT_CONNECTOR, prop_name,
					   property);
}

DrmPlane *DrmResources::GetPrimaryPlaneForCrtc(DrmCrtc &crtc)
{
	DrmPlane *primary_plane = NULL;
	for (auto &plane: planes_)
		if (plane->type() == DRM_PLANE_TYPE_PRIMARY &&
			plane->GetCrtcSupported(crtc))
			primary_plane = plane.get();

	if (!primary_plane)
		ALOGE("FATAL_ERROR: can't get primary plane for display %d",
			  crtc.display());

	return primary_plane;
}

}
