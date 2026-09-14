// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lvgl.h>
#include <vector>

namespace helix {

/**
 * @brief Clears a raw pointer the moment LVGL deletes the indev it names
 *
 * lv_evdev deletes its own device when a read fails, as it does when a USB
 * touch panel is unplugged, and lv_deinit() deletes every remaining indev at
 * shutdown. A caller holding a raw copy of that pointer outside the object
 * that opened it - DisplayManager's m_pointer/m_keyboard mirror whatever
 * display backend created them - has no way to learn the device is gone
 * unless something is listening for LV_EVENT_DELETE.
 *
 * Same technique as PointerFrameHook's owner_slot (pointer_frame_hook.h),
 * which solves the identical problem for the devices that hook fronts. This
 * is the version for a caller with no frame transform to chain: it never
 * touches the device's read callback, only its own LV_EVENT_DELETE listener.
 * Main thread only, like every indev.
 */
class IndevDeleteWatch {
  public:
    IndevDeleteWatch() = default;
    IndevDeleteWatch(const IndevDeleteWatch&) = delete;
    IndevDeleteWatch& operator=(const IndevDeleteWatch&) = delete;

    /// Stop watching every device still tracked.
    ~IndevDeleteWatch() {
        forget_all();
    }

    /**
     * @brief Clear *slot the moment LVGL deletes @p indev
     *
     * @param indev a live device, never nullptr
     * @param slot  the caller's own pointer to @p indev, cleared on delete
     */
    void watch(lv_indev_t* indev, lv_indev_t** slot) {
        lv_indev_add_event_cb(indev, on_deleted, LV_EVENT_DELETE, this);
        watches_.push_back({indev, slot});
    }

    /// Stop watching every device and forget them, without touching any slot.
    void forget_all() {
        for (const Watch& w : watches_) {
            lv_indev_remove_event_cb_with_user_data(w.indev, on_deleted, this);
        }
        watches_.clear();
    }

  private:
    struct Watch {
        lv_indev_t* indev;
        lv_indev_t** slot;
    };

    /// lv_indev_delete() sends this before it frees the device.
    static void on_deleted(lv_event_t* e) {
        auto* self = static_cast<IndevDeleteWatch*>(lv_event_get_user_data(e));
        const auto* target = static_cast<const lv_indev_t*>(lv_event_get_target(e));
        for (auto it = self->watches_.begin(); it != self->watches_.end();) {
            if (it->indev == target) {
                // Only while the slot still names this device - a caller may
                // already have pointed it at a fresh device (a backend swap)
                // before this delete event reaches it.
                if (it->slot != nullptr && *it->slot == target) {
                    *it->slot = nullptr;
                }
                it = self->watches_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<Watch> watches_;
};

} // namespace helix
