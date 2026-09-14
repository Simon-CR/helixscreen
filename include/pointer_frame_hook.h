// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "drm_rotation_strategy.h"
#include "indev_read_hook.h"
#include "input_device_scanner.h"
#include "touch_calibration.h"

#include <spdlog/spdlog.h>

#include <lvgl.h>
#include <optional>
#include <string>
#include <vector>

namespace helix {

/// What one pointer sample needs before LVGL's rotation step reads it
enum class PointerTransform {
    None,  ///< The sample is already in the frame LVGL rotates from
    Plane, ///< Turn it with the scanout plane, which LVGL's own rotation knows nothing about
};

/**
 * @brief Decide what a sample from a @p kind device needs
 *
 * A scanout plane that owns the rotation leaves LVGL's at zero and turns
 * everything LVGL draws. A touch panel reports where on the panel it was
 * touched, so its samples have to turn with the plane. A relative pointer's
 * driver accumulates motion into a position on the display LVGL lays out on,
 * and the plane turns that position along with the cursor drawn at it.
 *
 * @param kind           what the device is, from its own capabilities
 * @param plane_degrees  angle a scanout plane presents the picture at; 0 when none does
 */
inline PointerTransform pointer_transform_for(input::PointerKind kind, int plane_degrees) {
    if (kind == input::PointerKind::PanelAbsolute && plane_degrees != 0) {
        return PointerTransform::Plane;
    }
    return PointerTransform::None;
}

/**
 * @brief One read callback that hands LVGL each pointer device's samples in its frame
 *
 * Fronts every pointer device a backend opens, each recorded with the kind of
 * device it is, and applies pointer_transform_for() to each sample after that
 * device's own driver has produced it. The kind comes from the device, never
 * from which backend member holds it.
 *
 * The devices must outlive the hook's use of them, as for IndevReadHook: call
 * restore_all() before they are deleted. Main thread only, like every indev.
 */
class PointerFrameHook {
  public:
    PointerFrameHook() = default;
    PointerFrameHook(const PointerFrameHook&) = delete;
    PointerFrameHook& operator=(const PointerFrameHook&) = delete;

    /// A device still wrapped by the callback after this reads nothing.
    ~PointerFrameHook() {
        if (s_active == this) {
            s_active = nullptr;
        }
    }

    /**
     * @brief Put the hook in front of @p indev, a @p kind device
     *
     * @return true when this call put the hook in front of @p indev
     */
    bool install(lv_indev_t* indev, input::PointerKind kind) {
        if (!hook_.install(indev)) {
            return false;
        }
        devices_.push_back({indev, kind});
        s_active = this;
        return true;
    }

    /**
     * @brief Put the hook in front of @p indev, classified from the device it was opened on
     *
     * @param device_path the path the driver opened; see input::pointer_kind_for_device()
     * @return the kind @p indev was hooked as, or nullopt when this call did not hook it
     */
    std::optional<input::PointerKind> install(lv_indev_t* indev, const std::string& device_path) {
        const input::PointerKind kind = input::pointer_kind_for_device(device_path);
        if (!install(indev, kind)) {
            return std::nullopt;
        }
        return kind;
    }

    /**
     * @brief Record the angle a scanout plane presents the picture at
     *
     * @param degrees  0 when no plane rotates
     * @param panel_w  native panel width, before rotation
     * @param panel_h  native panel height, before rotation
     */
    void set_plane_rotation(int degrees, int32_t panel_w, int32_t panel_h) {
        plane_degrees_ = degrees;
        panel_w_ = panel_w;
        panel_h_ = panel_h;
    }

    /// Hand every device back the callback the hook replaced, then forget them.
    void restore_all() {
        hook_.restore_all();
        devices_.clear();
        if (s_active == this) {
            s_active = nullptr;
        }
    }

  private:
    struct Device {
        lv_indev_t* indev;
        input::PointerKind kind;
    };

    static void read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
        if (s_active != nullptr) {
            s_active->read(indev, data);
        }
    }

    void read(lv_indev_t* indev, lv_indev_data_t* data) const {
        const Device* device = find(indev);
        if (device == nullptr || !hook_.read_original(indev, data)) {
            return;
        }

        switch (pointer_transform_for(device->kind, plane_degrees_)) {
        case PointerTransform::None:
            return;
        case PointerTransform::Plane: {
            if (panel_w_ <= 0 || panel_h_ <= 0) {
                return;
            }
            const PointerXY raw{data->point.x, data->point.y};
            const PointerXY turned =
                rotate_pointer_for_plane(raw, plane_degrees_, panel_w_, panel_h_);
            data->point.x = turned.x;
            data->point.y = turned.y;
            if (is_touch_debug_enabled() && data->state == LV_INDEV_STATE_PRESSED) {
                spdlog::warn("[TouchDebug] plane_rotate {}°: raw=({},{}) -> screen=({},{}) "
                             "panel={}x{}",
                             plane_degrees_, raw.x, raw.y, turned.x, turned.y, panel_w_, panel_h_);
            }
            return;
        }
        }
    }

    const Device* find(const lv_indev_t* indev) const {
        for (const Device& device : devices_) {
            if (device.indev == indev) {
                return &device;
            }
        }
        return nullptr;
    }

    IndevReadHook hook_{read_cb};
    std::vector<Device> devices_;
    int plane_degrees_ = 0;
    int32_t panel_w_ = 0;
    int32_t panel_h_ = 0;

    /// LVGL stores only a function pointer, so the callback reaches the hook
    /// that installed last. One backend drives the display at a time.
    inline static PointerFrameHook* s_active = nullptr;
};

} // namespace helix
