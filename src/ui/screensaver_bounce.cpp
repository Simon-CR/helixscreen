// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_bounce.h"

#include "ui_confetti.h"
#include "ui_event_safety.h"
#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_utils.h"

#include "platform_capabilities.h"
#include "printer_image_manager.h"
#include "printer_images.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <draw/lv_image_decoder_private.h>
#include <string>

using helix::screensaver_bounce::fit_sprite;
using helix::screensaver_bounce::fold;
using helix::screensaver_bounce::fold_index;
using helix::screensaver_bounce::is_corner_hit;
using helix::screensaver_bounce::MIN_RANGE_PX;
using helix::screensaver_bounce::near_rational;
using helix::screensaver_bounce::sprite_size_for;

namespace {

// Tick period matches the rest of the subsystem: 50 ms (~20 fps) where the
// hardware has headroom, ~7 fps on BASIC/EMBEDDED so a screensaver the user
// opted back on stays clear of Klipper's print loop.
constexpr uint32_t TICK_PERIOD_STANDARD_MS = 50;
constexpr uint32_t TICK_PERIOD_LOW_MS = 150;

// Travel per second as a fraction of the screen's narrow axis, so a 480x272
// panel and a 1024x600 one read at the same pace.
constexpr float SPEED_FRACTION = 0.16f;

// Angles this far off both axes keep the path from degenerating into a line.
constexpr float ANGLE_MIN_DEG = 18.0f;
constexpr float ANGLE_SPAN_DEG = 54.0f;

// Rejection bound for short repeating paths, and how close counts as repeating.
constexpr int MAX_DENOMINATOR = 6;
constexpr float RATIONAL_EPS = 0.02f;
constexpr int SEED_ATTEMPTS = 16;

// Tint strength. At LV_OPA_COVER the recolor replaces every channel and the
// printer collapses to a flat silhouette; this leaves the render readable.
constexpr lv_opa_t RECOLOR_OPA = 110;

constexpr int CORNER_FLASH_TICKS = 3;
// How far the backdrop travels from black toward the tint. The overlay stays
// fully opaque throughout — fading it would show the UI it is covering.
constexpr lv_opa_t CORNER_FLASH_MIX = 56;
constexpr int CORNER_CONFETTI_PARTICLES = 40;

// Procedural palette for the sprite tint and the corner flash. These are
// animation content rather than UI chrome, so they are not theme tokens —
// the same footing as STAR_TINTS and CONFETTI_COLORS.
const lv_color_t BOUNCE_TINTS[] = {
    LV_COLOR_MAKE(0xFF, 0x6B, 0x6B), // red
    LV_COLOR_MAKE(0xFF, 0xE6, 0x6D), // yellow
    LV_COLOR_MAKE(0x4E, 0xCD, 0xC4), // teal
    LV_COLOR_MAKE(0x45, 0xB7, 0xD1), // blue
    LV_COLOR_MAKE(0x96, 0xE6, 0xA1), // green
    LV_COLOR_MAKE(0xDD, 0xA0, 0xDD), // plum
    LV_COLOR_MAKE(0xFF, 0xA6, 0x5C), // orange
};
constexpr int NUM_TINTS = sizeof(BOUNCE_TINTS) / sizeof(BOUNCE_TINTS[0]);

/// LVGL path of the image this printer should be represented by.
std::string resolve_sprite_path(int screen_w) {
    // An image the user picked by hand outranks whatever auto-detection settled
    // on, the same precedence the home screen widget applies.
    std::string path = helix::PrinterImageManager::instance().get_active_image_path(screen_w);
    if (!path.empty()) {
        return path;
    }
    return PrinterImages::get_best_printer_image(helix::get_saved_printer_type());
}

} // namespace

namespace helix {

void BouncingPrinterScreensaver::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Bouncing printer already active, ignoring start()");
        return;
    }

    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start bouncing printer");
        return;
    }

    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);

    const int box = sprite_size_for(screen_w_, screen_h_);
    if (box <= 0) {
        spdlog::warn("[Screensaver] {}x{} leaves under {}px of travel, declining to bounce",
                     screen_w_, screen_h_, MIN_RANGE_PX);
        return;
    }

    if (!decode_sprite()) {
        return;
    }
    fit_sprite(box, src_w_, src_h_, sprite_w_, sprite_h_);

    spdlog::info("[Screensaver] Starting bouncing printer");

    const auto caps = helix::PlatformCapabilities::detect();
    tick_period_ms_ = caps.supports_animations ? TICK_PERIOD_STANDARD_MS : TICK_PERIOD_LOW_MS;
    elapsed_ms_ = 0;

    rng_.seed(static_cast<std::mt19937::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count()));

    range_x_ = static_cast<float>(screen_w_ - sprite_w_);
    range_y_ = static_cast<float>(screen_h_ - sprite_h_);

    create_overlay();
    seed_motion();
    apply_tint();

    timer_ = lv_timer_create(tick_cb, tick_period_ms_, this);

    active_ = true;
    spdlog::debug(
        "[Screensaver] Bouncing printer started ({}x{}, {}x{} sprite, {}ms tick, {} tier)",
        screen_w_, screen_h_, sprite_w_, sprite_h_, tick_period_ms_,
        helix::platform_tier_to_string(caps.tier));
}

BouncingPrinterScreensaver::~BouncingPrinterScreensaver() {
    // ScreensaverManager owns these in a unique_ptr and does not stop the active
    // one before destroying it, so a screensaver torn down while running would
    // otherwise leave tick_cb armed on a freed `this` (#750, #751, #1173).
    cancel_timer();
}

void BouncingPrinterScreensaver::cancel_timer() {
    if (timer_) {
        helix::ui::lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }
}

void BouncingPrinterScreensaver::stop() {
    if (!active_) {
        return;
    }

    spdlog::info("[Screensaver] Stopping bouncing printer");

    cancel_timer();

    if (confetti_) {
        // Drops the particle timer now rather than waiting on the deferred
        // delete of the overlay it hangs from.
        ui_confetti_clear(confetti_);
        confetti_ = nullptr;
    }

    // Async delete — stop() runs inside lv_timer_handler (via check_display_sleep),
    // so synchronous deletion corrupts LVGL's event linked list (#316).
    helix::ui::safe_delete_deferred(overlay_);
    img_ = nullptr; // deleted as child of overlay

    free_sprite();

    prev_x_ = INT32_MIN;
    prev_y_ = INT32_MIN;
    corner_flash_ticks_ = 0;
    active_ = false;
}

bool BouncingPrinterScreensaver::decode_sprite() {
    const std::string path = resolve_sprite_path(screen_w_);

    // LV_CACHE_DEF_SIZE is 0, so an image handed to LVGL as a path is decoded
    // again on every redraw. Decoding once into a draw buffer makes each frame a
    // scaled blit over the sprite's own footprint and nothing more.
    lv_image_decoder_dsc_t dsc;
    lv_result_t res = lv_image_decoder_open(&dsc, path.c_str(), nullptr);
    if (res != LV_RESULT_OK || !dsc.decoded) {
        spdlog::warn("[Screensaver] Failed to decode printer image '{}'", path);
        if (res == LV_RESULT_OK) {
            lv_image_decoder_close(&dsc);
        }
        return false;
    }

    decoded_ = lv_draw_buf_dup(dsc.decoded);
    src_w_ = static_cast<int32_t>(dsc.decoded->header.w);
    src_h_ = static_cast<int32_t>(dsc.decoded->header.h);
    lv_image_decoder_close(&dsc);

    if (!decoded_) {
        spdlog::warn("[Screensaver] Failed to copy decoded printer image '{}'", path);
        return false;
    }

    // A device reads the pre-rendered variant here; a source tree without
    // `make gen-images` behind it holds the full-size art instead.
    spdlog::debug("[Screensaver] Bouncing printer sprite '{}' decoded at {}x{}", path, src_w_,
                  src_h_);
    return true;
}

void BouncingPrinterScreensaver::free_sprite() {
    if (decoded_) {
        lv_draw_buf_destroy(decoded_);
        decoded_ = nullptr;
    }
}

void BouncingPrinterScreensaver::create_overlay() {
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    // Clickable to absorb wake touch (prevents it from triggering underlying UI)
    // LVGL still registers the activity for inactivity tracking
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    img_ = lv_image_create(overlay_);
    lv_image_set_src(img_, decoded_);
    // CONTAIN makes the object's box and the sprite's footprint the same
    // rectangle, which is what the bounce arithmetic addresses. Set once here:
    // it forces a layout pass, and this overlay is built fresh on lv_layer_top()
    // rather than inside a live rebuild (#983/#1025).
    lv_image_set_inner_align(img_, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_size(img_, sprite_w_, sprite_h_);
    lv_obj_set_pos(img_, 0, 0);
}

void BouncingPrinterScreensaver::seed_motion() {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    const float narrow = static_cast<float>(std::min(screen_w_, screen_h_));
    const float speed = SPEED_FRACTION * narrow;

    // Start anywhere on the field so consecutive runs do not trace the same path.
    const float start_x = unit(rng_) * range_x_;
    const float start_y = unit(rng_) * range_y_;

    for (int attempt = 0; attempt < SEED_ATTEMPTS; ++attempt) {
        const float deg = ANGLE_MIN_DEG + unit(rng_) * ANGLE_SPAN_DEG;
        const float theta = deg * (static_cast<float>(M_PI) / 180.0f);
        const float sx = (unit(rng_) < 0.5f) ? -1.0f : 1.0f;
        const float sy = (unit(rng_) < 0.5f) ? -1.0f : 1.0f;

        vx_ = speed * std::cos(theta) * sx;
        vy_ = speed * std::sin(theta) * sy;

        if (range_x_ <= 0.0f || range_y_ <= 0.0f) {
            break;
        }
        const float ratio = std::fabs((vx_ / range_x_) / (vy_ / range_y_));
        if (!near_rational(ratio, MAX_DENOMINATOR, RATIONAL_EPS)) {
            break;
        }
    }

    x0_ = start_x;
    y0_ = start_y;
    prev_fold_x_ = fold_index(x0_, range_x_);
    prev_fold_y_ = fold_index(y0_, range_y_);

    // Half a tick of travel — wide enough that a corner is not missed between
    // frames, tight enough that a plain wall hit is not mistaken for one.
    const float per_tick = speed * static_cast<float>(tick_period_ms_) / 1000.0f;
    corner_tol_ = std::max(4.0f, per_tick / 2.0f);
}

void BouncingPrinterScreensaver::rebase(int screen_w, int screen_h) {
    const int box = sprite_size_for(screen_w, screen_h);
    if (box <= 0) {
        return; // keep bouncing in the old geometry rather than stop mid-flight
    }

    const float t = static_cast<float>(elapsed_ms_) / 1000.0f;
    const float cur_x = fold(x0_ + vx_ * t, range_x_);
    const float cur_y = fold(y0_ + vy_ * t, range_y_);

    screen_w_ = screen_w;
    screen_h_ = screen_h;
    fit_sprite(box, src_w_, src_h_, sprite_w_, sprite_h_);
    range_x_ = static_cast<float>(screen_w_ - sprite_w_);
    range_y_ = static_cast<float>(screen_h_ - sprite_h_);

    if (img_) {
        lv_obj_set_size(img_, sprite_w_, sprite_h_);
    }

    // Re-anchor the phase to where the sprite already is, so a rotation moves
    // the walls without teleporting what is between them.
    const float held_x = std::clamp(cur_x, 0.0f, range_x_);
    const float held_y = std::clamp(cur_y, 0.0f, range_y_);
    x0_ = held_x - vx_ * t;
    y0_ = held_y - vy_ * t;

    prev_fold_x_ = fold_index(x0_ + vx_ * t, range_x_);
    prev_fold_y_ = fold_index(y0_ + vy_ * t, range_y_);
    prev_x_ = INT32_MIN;
    prev_y_ = INT32_MIN;

    spdlog::debug("[Screensaver] Bouncing printer rebased to {}x{} ({}x{} sprite)", screen_w_,
                  screen_h_, sprite_w_, sprite_h_);
}

void BouncingPrinterScreensaver::apply_tint() {
    if (!img_) {
        return;
    }
    lv_obj_set_style_image_recolor(img_, BOUNCE_TINTS[color_index_], 0);
    lv_obj_set_style_image_recolor_opa(img_, RECOLOR_OPA, 0);
}

void BouncingPrinterScreensaver::tick_cb(lv_timer_t* timer) {
    LVGL_SAFE_EVENT_CB_BEGIN("[BouncingPrinterScreensaver] tick_cb");

    auto* self = static_cast<BouncingPrinterScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->active_) {
        return;
    }
    self->tick();

    LVGL_SAFE_EVENT_CB_END();
}

void BouncingPrinterScreensaver::tick() {
    elapsed_ms_ += tick_period_ms_;

    lv_display_t* disp = lv_display_get_default();
    if (disp) {
        const int w = lv_display_get_horizontal_resolution(disp);
        const int h = lv_display_get_vertical_resolution(disp);
        if (w != screen_w_ || h != screen_h_) {
            rebase(w, h);
        }
    }

    const float t = static_cast<float>(elapsed_ms_) / 1000.0f;
    const float ux = x0_ + vx_ * t;
    const float uy = y0_ + vy_ * t;

    const float x = fold(ux, range_x_);
    const float y = fold(uy, range_y_);

    const int fx = fold_index(ux, range_x_);
    const int fy = fold_index(uy, range_y_);
    const bool bounced_x = (fx != prev_fold_x_);
    const bool bounced_y = (fy != prev_fold_y_);
    prev_fold_x_ = fx;
    prev_fold_y_ = fy;

    if (bounced_x || bounced_y) {
        color_index_ = (color_index_ + 1) % NUM_TINTS;
        apply_tint();
    }

    const bool corner = is_corner_hit(bounced_x, bounced_y, x, y, range_x_, range_y_, corner_tol_);
    if (corner) {
        spdlog::info("[Screensaver] Bouncing printer hit the corner");
        corner_flash_ticks_ = CORNER_FLASH_TICKS;
        lv_obj_set_style_bg_color(
            overlay_, lv_color_mix(BOUNCE_TINTS[color_index_], lv_color_black(), CORNER_FLASH_MIX),
            0);

        // The particle system is 60 objects repositioned and resized every
        // frame. That is the cost profile BASIC and EMBEDDED tiers had the
        // screensaver taken away over, so those get the flash alone.
        if (helix::PlatformCapabilities::detect().supports_animations) {
            if (!confetti_) {
                confetti_ = ui_confetti_create(overlay_);
            }
            if (confetti_) {
                ui_confetti_burst(confetti_, CORNER_CONFETTI_PARTICLES);
            }
        }
    } else if (corner_flash_ticks_ > 0 && --corner_flash_ticks_ == 0) {
        lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    }

    const int32_t px = static_cast<int32_t>(std::lround(x));
    const int32_t py = static_cast<int32_t>(std::lround(y));
    if (px != prev_x_ || py != prev_y_) {
        lv_obj_set_pos(img_, px, py);
        prev_x_ = px;
        prev_y_ = py;
    }
}

} // namespace helix

#endif // HELIX_ENABLE_SCREENSAVER
