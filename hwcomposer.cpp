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

#define LOG_TAG "hwcomposer-drm-nexell"

#include <stdlib.h>

#include <cinttypes>
#include <map>
#include <vector>
#include <sstream>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <linux/fb.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cutils/log.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <sw_sync.h>
#include <sync/sync.h>

#include <gralloc_priv.h>

#include "drmresources.h"
#include "drmeventlistener.h"
#include "vsyncworker.h"
#include "importer.h"
#include "renderworker.h"

#define UM_PER_INCH 25400

namespace android {

typedef struct hwc_drm_display {
    struct hwc_context_t *ctx;
    int display;

    std::vector<uint32_t> config_ids;

    VSyncWorker vsync_worker;
    RenderWorker render_worker;

    hwc_drm_bo_t *bo[NUM_FB_BUFFERS];

    DrmMode active_mode;
    bool needs_modeset;
    uint32_t blob_id;
    uint32_t old_blob_id;
} hwc_drm_display_t;

static int hwc_set_display_active_mode(struct hwc_context_t *ctx, int display,
                                       DrmMode &mode);

static void hwc_release_display(struct hwc_context_t *ctx, int display);

class DrmHotplugHandler: public DrmEventHandler {
public:
    void Init(DrmResources *drm, struct hwc_context_t *ctx,
          const struct hwc_procs *procs) {
        drm_ = drm;
        ctx_ = ctx;
        procs_ = procs;
    }

    void HandleEvent(uint64_t timestamp_us) {
        for (auto &conn : drm_->connectors()) {
            drmModeConnection old_state = conn->state();

            conn->UpdateModes();

            drmModeConnection cur_state = conn->state();

            if (cur_state == old_state)
                continue;

            ALOGI("%s event @%" PRIu64 " for connector %u\n",
                  cur_state == DRM_MODE_CONNECTED ? "Plug" : "Unplug",
                  timestamp_us, conn->id());

            if (cur_state == DRM_MODE_CONNECTED) {
                // Take the first one, then look for the preferred
                DrmMode mode = *(conn->modes().begin());
                for (auto &m : conn->modes()) {
                    if (m.type() & DRM_MODE_TYPE_PREFERRED) {
                        mode = m;
                        break;
                    }
                }
                ALOGI("Setting mode %dx%d for connector %d\n", mode.h_display(),
                      mode.v_display(), conn->id());
                int ret = hwc_set_display_active_mode(ctx_, conn->display(),
                                                      mode);
                if (ret) {
                    ALOGE("Failed to set active config %d", ret);
                    return;
                }
            } else {
                int ret = drm_->SetDpmsMode(conn->display(), DRM_MODE_DPMS_OFF);
                if (ret) {
                    ALOGE("Failed to set dpms mode off %d", ret);
                    return;
                }

                if (conn->display() == 1) {
                    /* HDMI */
                    ALOGV("HDMI Disconnected");
                    hwc_release_display(ctx_, conn->display());
                }
            }

            procs_->hotplug(procs_, conn->display(),
                           cur_state == DRM_MODE_CONNECTED ? 1 : 0);
        }
    }

private:
    DrmResources *drm_ = NULL;
    const struct hwc_procs *procs_ = NULL;
    struct hwc_context_t *ctx_ = NULL;
};

struct hwc_context_t {
    typedef std::map<int, hwc_drm_display_t> DisplayMap;

    hwc_composer_device_1_t device;
    hwc_procs_t const *procs = NULL;

    DisplayMap displays;
    DrmResources drm;
    DrmHotplugHandler hotplug_handler;
    Importer *importer;
    private_module_t *gralloc;
};

RenderWorker::~RenderWorker()
{
}

void RenderWorker::Routine()
{
    if (queue_.isEmpty()) {
        int ret = Lock();

        if (ret)
            ALOGE("Failed to lock render worker %d", ret);

        int wait_ret = WaitForSignalOrExitLocked();

        switch (wait_ret) {
        case 0:
            break;
        default:
            ALOGE("RenderWorker failed to wait for signal %d", wait_ret);
            break;
        }

        ret = Unlock();
        if (ret)
            ALOGE("Failed to unlock worker %d", ret);
    }

    buffer_handle_t h = DequeueFB();
    if (h != NULL)
        Render(h);
}

int RenderWorker::Render(buffer_handle_t handle)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)ctx_;

    hwc_drm_display_t *hd = &ctx->displays[id_];
    hwc_drm_bo_t *bo = NULL;
    int ret;

    for (int i = 0; i < NUM_FB_BUFFERS; i++) {
        if (hd->bo[i] &&
            hd->bo[i]->priv == (void *)(handle)) {
            bo = hd->bo[i];
            break;
        }
    }

    if (!bo) {
        bo = new hwc_drm_bo_t();

        ret = ctx->importer->ImportBuffer(handle, bo);
        if (ret) {
            ALOGE("FATAL ERROR: failed to ImportBuffer for %p", handle);
            return ret;
        }

        for (int i = 0; i < NUM_FB_BUFFERS; i++) {
            if (!hd->bo[i]) {
                hd->bo[i] = bo;
                break;
            }
        }
    }

    DrmCrtc *crtc = ctx->drm.GetCrtcForDisplay(id_);
    if (!crtc) {
        ALOGE("FATAL ERROR: can't get crtc for display %d", id_);
        return -ENODEV;
    }

    DrmPlane *plane = ctx->drm.GetPrimaryPlaneForCrtc(*crtc);
    if (!plane) {
        ALOGE("FATAL ERROR: can't get primary plane for display %d", id_);
        return -ENODEV;
    }

    drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
    if (!pset) {
        ALOGE("Failed to allocate property set");
        return -ENOMEM;
    }

    DrmConnector *connector = NULL;
    connector = ctx->drm.GetConnectorForDisplay(id_);
    if (!connector) {
        ALOGE("Could not locate connector for display %d", id_);
        return -ENODEV;
    }
    if (hd->needs_modeset) {
        ret = drmModeAtomicAddProperty(pset, crtc->id(),
                                       crtc->mode_property().id(),
                                       hd->blob_id);
        if (ret < 0) {
            ALOGE("Failed to add blob %d to pset", hd->blob_id);
            drmModeAtomicFree(pset);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, connector->id(),
                                       connector->crtc_id_property().id(),
                                       crtc->id());
        if (ret < 0) {
            ALOGE("Failed to add conn/crtc id property to pset");
            drmModeAtomicFree(pset);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_property().id(),
                                       crtc->id());
        if (ret < 0) {
            ALOGE("Failed to add crtc id property for plane %d, ret %d",
                  plane->id(), ret);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_x_property().id(),
                                       displayFrame_.left);
        if (ret < 0) {
            ALOGE("Failed to add x property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_y_property().id(),
                                       displayFrame_.top);
        if (ret < 0) {
            ALOGE("Failed to add y property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_w_property().id(),
                                       displayFrame_.right -
                                       displayFrame_.left);
        if (ret < 0) {
            ALOGE("Failed to add w property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_h_property().id(),
                                       displayFrame_.bottom -
                                       displayFrame_.top);
        if (ret < 0) {
            ALOGE("Failed to add h property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_x_property().id(),
                                       displayFrame_.left);
        if (ret < 0) {
            ALOGE("Failed to add src x property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_y_property().id(),
                                       displayFrame_.top);
        if (ret < 0) {
            ALOGE("Failed to add src y property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_w_property().id(),
                                       displayFrame_.right -
                                       displayFrame_.left);
        if (ret < 0) {
            ALOGE("Failed to add src w property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_h_property().id(),
                                       displayFrame_.bottom -
                                       displayFrame_.top);
        if (ret < 0) {
            ALOGE("Failed to add src h property for plane %d", plane->id());
            return ret;
        }
    }

    ret = drmModeAtomicAddProperty(pset, plane->id(),
                                   plane->fb_property().id(),
                                   bo->fb_id);
    if (ret < 0) {
        ALOGE("Failed to add fb_id(%d) property for plane %d", bo->fb_id,
              plane->id());
        return ret;
    }
    uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    ret = drmModeAtomicCommit(ctx->drm.fd(), pset, flags, &ctx->drm);
    if (ret) {
        ALOGE("Failed to commit pset ret=%d\n", ret);
        drmModeAtomicFree(pset);
        return ret;
    }

    drmModeAtomicFree(pset);

    if (hd->needs_modeset) {
        ret = ctx->drm.DestroyPropertyBlob(hd->old_blob_id);
        if (ret) {
            ALOGE("Failed to destroy old blob id %" PRIu32 "/%d",
                  hd->old_blob_id, ret);
            return ret;
        }

        connector->set_active_mode(hd->active_mode);
        hd->old_blob_id = hd->blob_id;
        hd->needs_modeset = false;

        return ctx->drm.SetDpmsMode(id_, DRM_MODE_DPMS_ON);
    }

    return 0;
}

static void hwc_release_display(struct hwc_context_t *ctx, int display)
{
    hwc_drm_display *hd = &ctx->displays[display];

    /* Release All bo */
    for (int i = 0; i < NUM_FB_BUFFERS; i++) {
        if (hd->bo[i]) {
            ctx->importer->ReleaseBuffer(hd->bo[i]);
            delete hd->bo[i];
            hd->bo[i] = NULL;
        }
    }
}

static int hwc_set_display_active_mode(struct hwc_context_t *ctx, int display,
                                       DrmMode &mode)
{
    DrmConnector *connector = ctx->drm.GetConnectorForDisplay(display);
    if (!connector) {
        ALOGE("Could not locate connector for display %d", display);
        return -ENODEV;
    }

    hwc_drm_display_t *hd = &ctx->displays[display];
    
    struct drm_mode_modeinfo drm_mode;
    memset(&drm_mode, 0, sizeof(drm_mode));
    mode.ToDrmModeModeInfo(&drm_mode);

    uint32_t id = 0;
    int ret = ctx->drm.CreatePropertyBlob(&drm_mode,
                                          sizeof(struct drm_mode_modeinfo),
                                          &id);
    if (ret) {
        ALOGE("Failed to create mode property blob %d", ret);
        return ret;
    }

    hd->needs_modeset = true;
    hd->blob_id = id;
    hd->active_mode = mode;

    connector->set_active_mode(hd->active_mode);
    return 0;
}

////////////////////////////////////////////////////////
// Implement HWComposer Callback
////////////////////////////////////////////////////////
static int hwc_get_display_configs(struct hwc_composer_device_1 *dev,
                                   int display, uint32_t *configs,
                                   size_t *num_configs)
{
    if (!*num_configs)
        return 0;

    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    hwc_drm_display_t *hd = &ctx->displays[display];
    hd->config_ids.clear();

    DrmConnector *connector = ctx->drm.GetConnectorForDisplay(display);
    if (!connector) {
        ALOGV("Failed to get connector for display %d", display);
        return -ENODEV;
    };

    int ret = connector->UpdateModes();
    if (ret) {
        ALOGE("Failed to update display modes %d", ret);
        return ret;
    }

    for (const DrmMode &mode : connector->modes()) {
        size_t idx = hd->config_ids.size();
        if (idx == *num_configs)
            break;
        hd->config_ids.push_back(mode.id());
        configs[idx] = mode.id();
    }
    *num_configs = hd->config_ids.size();
    return *num_configs == 0 ? -1 : 0;
}

static int hwc_set_active_config(struct hwc_composer_device_1 *dev, int display,
                                 int index)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    hwc_drm_display_t *hd = &ctx->displays[display];
    if (index >= (int)hd->config_ids.size()) {
        ALOGE("Invalid config index %d passed in", index);
        return -EINVAL;
    }

    DrmConnector *c = ctx->drm.GetConnectorForDisplay(display);
    if (!c) {
        ALOGE("Failed to get connector for display %d", display);
        return -ENODEV;
    }

    if (c->state() != DRM_MODE_CONNECTED)
        return -ENODEV;

    DrmMode mode;
    for (const DrmMode &conn_mode : c->modes()) {
        if (conn_mode.id() == hd->config_ids[index]) {
            mode = conn_mode;
            break;
        }
    }
    if (mode.id() != hd->config_ids[index]) {
        ALOGE("Could not find active mode for %d/%d", index,
              hd->config_ids[index]);
        return -ENOENT;
    }

    int ret = hwc_set_display_active_mode(ctx, display, mode);
    if (ret) {
        ALOGE("Failed to set active config %d", ret);
        return ret;
    }

    return ret;
}

static int hwc_set_initial_config(hwc_drm_display_t *hd)
{
    uint32_t config;
    size_t num_configs = 1;
    int ret = hwc_get_display_configs(&hd->ctx->device, hd->display, &config,
                                      &num_configs);
    if (ret || !num_configs)
        return 0;

    ret = hwc_set_active_config(&hd->ctx->device, hd->display, 0);
    if (ret) {
        ALOGE("Failed to set active config d=%d ret=%d", hd->display, ret);
        return ret;
    }

    return ret;
}

static int hwc_initialize_display(struct hwc_context_t *ctx, int display)
{
    hwc_drm_display_t *hd = &ctx->displays[display];
    hd->ctx = ctx;
    hd->display = display;

    int ret = hwc_set_initial_config(hd);
    if (ret) {
        ALOGE("Failed to set initial config for d=%d ret=%d", display, ret);
        return ret;
    }

    ret = hd->vsync_worker.Init(&ctx->drm, display);
    if (ret) {
        ALOGE("Failed to create event worker for display %d %d\n", display,
              ret);
        return ret;
    }

    ret = hd->render_worker.Init(display, ctx);
    if (ret) {
        ALOGE("Failed to create render worker for display %d %d\n", display,
              ret);
        return ret;
    }

    return 0;
}

static int hwc_enumerate_displays(struct hwc_context_t *ctx)
{
    int ret;

    for (auto &conn : ctx->drm.connectors()) {
        ret = hwc_initialize_display(ctx, conn->display());
        if (ret) {
            ALOGE("Failed to initialize display %d", conn->display());
            return ret;
        }
    }

    return 0;
}

static int hwc_device_close(struct hw_device_t *dev)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)dev;
    delete ctx;
    return 0;
}

static int hwc_prepare(hwc_composer_device_1_t * dev __attribute__((unused)),
                       size_t num_displays,
                       hwc_display_contents_1_t **display_contents)
{
    for (int i = 0; i < (int)num_displays; ++i) {
        if (!display_contents[i])
            continue;

        int num_layers = display_contents[i]->numHwLayers;
        for (int j = 0; j < num_layers; ++j) {
            hwc_layer_1_t *layer = &display_contents[i]->hwLayers[j];

            switch (layer->compositionType) {
            case HWC_OVERLAY:
            case HWC_BACKGROUND:
            case HWC_SIDEBAND:
            case HWC_CURSOR_OVERLAY:
                layer->compositionType = HWC_FRAMEBUFFER;
                break;
            }
        }
    }

    return 0;
}

static int render_fb(struct hwc_context_t *ctx, int display,
                     hwc_layer_1_t *fb_layer)
{
    hwc_drm_display_t *hd = &ctx->displays[display];
    hwc_drm_bo_t *bo = NULL;
    int ret;
    ALOGI("[SEOJI] render_fb");

    if (!fb_layer->handle)
        return -EINVAL;

    for (int i = 0; i < NUM_FB_BUFFERS; i++) {
        if (hd->bo[i] &&
            hd->bo[i]->priv == (void *)(fb_layer->handle)) {
            bo = hd->bo[i];
            break;
        }
    }

    if (!bo) {
        bo = new hwc_drm_bo_t();

        ret = ctx->importer->ImportBuffer(fb_layer->handle, bo);
        if (ret) {
            ALOGE("FATAL ERROR: failed to ImportBuffer for %p",
                  fb_layer->handle);
            return ret;
        }

        for (int i = 0; i < NUM_FB_BUFFERS; i++) {
            if (!hd->bo[i]) {
                hd->bo[i] = bo;
                break;
            }
        }
    }

    DrmCrtc *crtc = ctx->drm.GetCrtcForDisplay(display);
    if (!crtc) {
        ALOGE("FATAL ERROR: can't get crtc for display %d", display);
        return -ENODEV;
    }

    DrmPlane *plane = ctx->drm.GetPrimaryPlaneForCrtc(*crtc);
    if (!plane) {
        ALOGE("FATAL ERROR: can't get primary plane for display %d", display);
        return -ENODEV;
    }

    drmModeAtomicReqPtr pset = drmModeAtomicAlloc();
    if (!pset) {
        ALOGE("Failed to allocate property set");
        return -ENOMEM;
    }

    DrmConnector *connector = NULL;
    connector = ctx->drm.GetConnectorForDisplay(display);
    if (!connector) {
        ALOGE("Could not locate connector for display %d", display);
        return -ENODEV;
    }
    if (hd->needs_modeset) {
        ret = drmModeAtomicAddProperty(pset, crtc->id(),
                                       crtc->mode_property().id(),
                                       hd->blob_id);
        if (ret < 0) {
            ALOGE("Failed to add blob %d to pset", hd->blob_id);
            drmModeAtomicFree(pset);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, connector->id(),
                                       connector->crtc_id_property().id(),
                                       crtc->id());
        if (ret < 0) {
            ALOGE("Failed to add conn/crtc id property to pset");
            drmModeAtomicFree(pset);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_property().id(),
                                       crtc->id());
        if (ret < 0) {
            ALOGE("Failed to add crtc id property for plane %d, ret %d",
                  plane->id(), ret);
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_x_property().id(),
                                       fb_layer->displayFrame.left);
        if (ret < 0) {
            ALOGE("Failed to add x property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_y_property().id(),
                                       fb_layer->displayFrame.top);
        if (ret < 0) {
            ALOGE("Failed to add y property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_w_property().id(),
                                       fb_layer->displayFrame.right -
                                       fb_layer->displayFrame.left);
        if (ret < 0) {
            ALOGE("Failed to add w property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->crtc_h_property().id(),
                                       fb_layer->displayFrame.bottom -
                                       fb_layer->displayFrame.top);
        if (ret < 0) {
            ALOGE("Failed to add h property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_x_property().id(),
                                       fb_layer->displayFrame.left);
        if (ret < 0) {
            ALOGE("Failed to add src x property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_y_property().id(),
                                       fb_layer->displayFrame.top);
        if (ret < 0) {
            ALOGE("Failed to add src y property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_w_property().id(),
                                       fb_layer->displayFrame.right -
                                       fb_layer->displayFrame.left);
        if (ret < 0) {
            ALOGE("Failed to add src w property for plane %d", plane->id());
            return ret;
        }

        ret = drmModeAtomicAddProperty(pset, plane->id(),
                                       plane->src_h_property().id(),
                                       fb_layer->displayFrame.bottom -
                                       fb_layer->displayFrame.top);
        if (ret < 0) {
            ALOGE("Failed to add src h property for plane %d", plane->id());
            return ret;
        }
    }

    ALOGV("fb_id: %d", bo->fb_id);
    ret = drmModeAtomicAddProperty(pset, plane->id(),
                                   plane->fb_property().id(),
                                   bo->fb_id);
    if (ret < 0) {
        ALOGE("Failed to add fb_id(%d) property for plane %d", bo->fb_id,
              plane->id());
        return ret;
    }

    // sync fence
    if (fb_layer->acquireFenceFd >= 0) {
    	ALOGI("[SEOJI] sync_wait");
        sync_wait(fb_layer->acquireFenceFd, 1000);
	}

    uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    ret = drmModeAtomicCommit(ctx->drm.fd(), pset, flags, &ctx->drm);
    if (fb_layer->acquireFenceFd >= 0) {
        close(fb_layer->acquireFenceFd);
        fb_layer->acquireFenceFd = -1;
    }
    if (ret) {
        ALOGE("Failed to commit pset ret=%d\n", ret);
        drmModeAtomicFree(pset);
        return ret;
    }

    drmModeAtomicFree(pset);

    if (hd->needs_modeset) {
        ret = ctx->drm.DestroyPropertyBlob(hd->old_blob_id);
        if (ret) {
            ALOGE("Failed to destroy old blob id %" PRIu32 "/%d",
                  hd->old_blob_id, ret);
            return ret;
        }

        connector->set_active_mode(hd->active_mode);
        hd->old_blob_id = hd->blob_id;
        hd->needs_modeset = false;

        return ctx->drm.SetDpmsMode(display, DRM_MODE_DPMS_ON);
    }

    return 0;
}

static int hwc_set(hwc_composer_device_1_t * dev, size_t num_displays,
                   hwc_display_contents_1_t ** sf_display_contents)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    int ret = 0;
    ALOGI("[SEOJI] hwc_set");

    for (size_t i = 0; i < num_displays; i++) {
        hwc_display_contents_1_t *dc = sf_display_contents[i];

        if (!dc || i == HWC_DISPLAY_VIRTUAL)
            continue;

        hwc_layer_1_t *fb_layer = &dc->hwLayers[dc->numHwLayers - 1];

        ret = render_fb(ctx, i, fb_layer);
        if (ret)
            ALOGV("failed to render_fb for display %zu", i);
    }

    return 0;
}

static int __attribute__((unused))
hwc_set_framebuffer_target(struct hwc_composer_device_1 *dev, int32_t id,
                           hwc_layer_1_t *layer)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;

    if (id == 0) {
        render_fb(ctx, id, layer);
    } else {
        hwc_drm_display_t *hd = &ctx->displays[id];
        hd->render_worker.SetDisplayFrame(layer->displayFrame);
        hd->render_worker.QueueFB(layer->handle);
    }

    return 0;
}

static int hwc_event_control(struct hwc_composer_device_1 *dev, int display,
                             int event, int enabled)
{
    if (event != HWC_EVENT_VSYNC || (enabled != 0 && enabled != 1))
        return -EINVAL;

    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    hwc_drm_display_t *hd = &ctx->displays[display];
    return hd->vsync_worker.VSyncControl(enabled);
}

static int hwc_set_power_mode(struct hwc_composer_device_1 *dev, int display,
                              int mode)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;

    uint64_t dpmsValue = 0;
    switch (mode) {
    case HWC_POWER_MODE_OFF:
        dpmsValue = DRM_MODE_DPMS_OFF;
        break;

    /* TODO: We can't support dozing right now, so go full on */
    case HWC_POWER_MODE_DOZE:
    case HWC_POWER_MODE_DOZE_SUSPEND:
    case HWC_POWER_MODE_NORMAL:
        dpmsValue = DRM_MODE_DPMS_ON;
        break;
    }

    // HACK: If calling SetDpmsMode here, HDMI is not working...
    if (display == HWC_DISPLAY_PRIMARY)
        return ctx->drm.SetDpmsMode(display, dpmsValue);

    return 0;
}

static int hwc_query(struct hwc_composer_device_1 * /* dev */, int what,
                     int *value)
{
    switch (what) {
    case HWC_BACKGROUND_LAYER_SUPPORTED:
        *value = 0; /* TODO: We should do this */
        break;
    case HWC_VSYNC_PERIOD:
        ALOGW("Query for deprecated vsync value, returning 60Hz");
        *value = 1000 * 1000 * 1000 / 60;
        break;
    case HWC_DISPLAY_TYPES_SUPPORTED:
        *value = HWC_DISPLAY_PRIMARY_BIT | HWC_DISPLAY_EXTERNAL_BIT |
                 HWC_DISPLAY_VIRTUAL_BIT;
        break;
    }
    return 0;
}

static void hwc_register_procs(struct hwc_composer_device_1 *dev,
                               hwc_procs_t const *procs)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;

    ctx->procs = procs;

    for (std::pair<const int, hwc_drm_display> &display_entry : ctx->displays)
        display_entry.second.vsync_worker.SetProcs(procs);

    ctx->hotplug_handler.Init(&ctx->drm, ctx, procs);
    ctx->drm.event_listener()->RegisterHotplugHandler(&ctx->hotplug_handler);
}

static int hwc_get_display_attributes(struct hwc_composer_device_1 *dev,
                                      int display, uint32_t config,
                                      const uint32_t *attributes,
                                      int32_t *values)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    DrmConnector *c = ctx->drm.GetConnectorForDisplay(display);
    if (!c) {
        ALOGE("Failed to get DrmConnector for display %d", display);
        return -ENODEV;
    }
    DrmMode mode;
    for (const DrmMode &conn_mode : c->modes()) {
        if (conn_mode.id() == config) {
            mode = conn_mode;
            break;
        }
    }
    if (mode.id() == 0) {
        ALOGE("Failed to find active mode for display %d", display);
        return -ENOENT;
    }

    uint32_t mm_width = c->mm_width();
    uint32_t mm_height = c->mm_height();
    for (int i = 0; attributes[i] != HWC_DISPLAY_NO_ATTRIBUTE; ++i) {
        switch (attributes[i]) {
        case HWC_DISPLAY_VSYNC_PERIOD:
            values[i] = 1000 * 1000 * 1000 / mode.v_refresh();
            break;
        case HWC_DISPLAY_WIDTH:
            values[i] = mode.h_display();
            break;
        case HWC_DISPLAY_HEIGHT:
            values[i] = mode.v_display();
            break;
        case HWC_DISPLAY_DPI_X:
            /* Dots per 1000 inches */
            values[i] =
                mm_width ? (mode.h_display() * UM_PER_INCH) / mm_width : 0;
            break;
        case HWC_DISPLAY_DPI_Y:
            /* Dots per 1000 inches */
            values[i] =
                mm_height ? (mode.v_display() * UM_PER_INCH) / mm_height : 0;
            break;
        }
    }
    return 0;
}

static int hwc_get_active_config(struct hwc_composer_device_1 *dev, int display)
{
    struct hwc_context_t *ctx = (struct hwc_context_t *)&dev->common;
    DrmConnector *c = ctx->drm.GetConnectorForDisplay(display);
    if (!c) {
        ALOGE("Failed to get DrmConnector for display %d", display);
        return -ENODEV;
    }

    DrmMode mode = c->active_mode();
    hwc_drm_display_t *hd = &ctx->displays[display];
    for (size_t i = 0; i < hd->config_ids.size(); ++i)
        if (hd->config_ids[i] == mode.id())
            return i;

    return -1;
}

static int hwc_device_open(const struct hw_module_t *module, const char *name,
                           struct hw_device_t **dev)
{
    if (strcmp(name, HWC_HARDWARE_COMPOSER)) {
        ALOGE("Invalid module name %s", name);
        return -EINVAL;
    }

    std::unique_ptr<hwc_context_t> ctx(new hwc_context_t());
    if (!ctx) {
        ALOGE("Failed to allocate hwc context");
        return -ENOMEM;
    }

    int ret = ctx->drm.Init();
    if (ret) {
        ALOGE("Can't initialize Drm object %d", ret);
        return ret;
    }

    ret = hwc_enumerate_displays(ctx.get());
    if (ret) {
        ALOGE("Failed to enumerate displays: %s", strerror(ret));
        return ret;
    }

    ctx->importer = Importer::CreateInstance(&ctx->drm);
    if (!ctx->importer) {
        ALOGE("Failed to CreateInstance for importer\n");
        return -ENOENT;
    }

    hw_module_t *gralloc;
    ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                (const hw_module_t **)&gralloc);
    if (ret) {
        ALOGE("Failed to get gralloc module");
        return ret;
    }
    ctx->gralloc = reinterpret_cast<private_module_t *>(gralloc);

    ctx->device.common.tag = HARDWARE_DEVICE_TAG;
    ctx->device.common.version = HWC_DEVICE_API_VERSION_1_4;
    ctx->device.common.module = const_cast<hw_module_t *>(module);
    ctx->device.common.close = hwc_device_close;

    ctx->device.prepare = hwc_prepare;
    ctx->device.set = hwc_set;
    // below is code for libhardware hwcomposer hal nexell extension
    // ctx->device.setFramebufferTarget = hwc_set_framebuffer_target;
    ctx->device.eventControl = hwc_event_control;
    ctx->device.setPowerMode = hwc_set_power_mode;
    ctx->device.query = hwc_query;
    ctx->device.registerProcs = hwc_register_procs;
    ctx->device.getDisplayConfigs = hwc_get_display_configs;
    ctx->device.getDisplayAttributes = hwc_get_display_attributes;
    ctx->device.getActiveConfig = hwc_get_active_config;
    ctx->device.setActiveConfig = hwc_set_active_config;
    ctx->device.setCursorPositionAsync = NULL; /* TODO: Add cursor */

    *dev = &ctx->device.common;
    ctx.release();

    return 0;
}

}

static struct hw_module_methods_t hwc_module_methods = {
    .open = android::hwc_device_open
};

hwc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = HWC_HARDWARE_MODULE_ID,
        .name = "Nexell DRM hwcomposer module",
        .author = "Sungwoo Park <swpark@nexell.co.kr>",
        .methods = &hwc_module_methods,
        .dso = NULL,
        .reserved = {0},
    }
};
