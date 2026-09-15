// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver_starfield.h"

#include "ui_timer_guard.h" // lv_timer_cancel_safe
#include "ui_utils.h"

#include <spdlog/spdlog.h>

#include <ctime>

using helix::ui::FrameTarget;
using helix::ui::SCREENSAVER_CANVAS_FORMAT;
using helix::ui::StarfieldSim;

static_assert(LV_COLOR_DEPTH == 32, "the starfield canvas is XRGB8888 on a 32 bpp display");

void StarfieldScreensaver::start() {
    if (active_) {
        spdlog::debug("[Screensaver] Starfield already active, ignoring start()");
        return;
    }

    spdlog::info("[Screensaver] Starting starfield");

    lv_display_t* disp = lv_display_get_default();
    if (!disp) {
        spdlog::warn("[Screensaver] No display available, cannot start starfield");
        return;
    }

    screen_w_ = lv_display_get_horizontal_resolution(disp);
    screen_h_ = lv_display_get_vertical_resolution(disp);

    // Create black overlay on lv_layer_top() — absorbs touch input
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // Create canvas as child of overlay
    canvas_ = lv_canvas_create(overlay_);
    lv_obj_set_size(canvas_, screen_w_, screen_h_);
    lv_obj_set_pos(canvas_, 0, 0);

    // Allocate the draw buffer at LVGL's row stride: lv_canvas_set_buffer()
    // steps rows by the aligned stride and lv_canvas_fill_bg() writes the full
    // extent immediately, so a tightly-packed w * h * 4 allocation under-runs it.
    draw_buf_stride_ =
        helix::ui::screensaver_canvas_stride_bytes(screen_w_, SCREENSAVER_CANVAS_FORMAT);
    size_t buf_size = static_cast<size_t>(draw_buf_stride_) * screen_h_;
    draw_buf_ = static_cast<uint8_t*>(lv_malloc(buf_size));
    if (!draw_buf_) {
        spdlog::error("[Screensaver] Failed to allocate {}KB draw buffer", buf_size / 1024);
        helix::ui::safe_delete_deferred(overlay_);
        canvas_ = nullptr;
        return;
    }
    draw_buf_size_ = buf_size;

    lv_canvas_set_buffer(canvas_, draw_buf_, screen_w_, screen_h_, SCREENSAVER_CANVAS_FORMAT);
    lv_canvas_fill_bg(canvas_, lv_color_black(), LV_OPA_COVER);
    // The opaque canvas covers the whole overlay. Top-layer children are never
    // cover-culled, so an opaque overlay background would be filled under it every frame.
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_TRANSP, 0);

    // Seed the owned random sequence and place the stars
    rng_.seed(fixed_seed_.value_or(static_cast<uint32_t>(time(nullptr))));
    sim_.init(static_cast<uint32_t>(screen_w_), static_cast<uint32_t>(screen_h_), rng_);

    // Create render timer at the display refresh period
    clock_.reset(lv_tick_get());
    timer_ = lv_timer_create(frame_timer_cb, helix::ui::screensaver_timer_period_ms(), this);

    active_ = true;
    spdlog::debug("[Screensaver] Starfield started ({}x{}, {} stars)", screen_w_, screen_h_,
                  StarfieldSim::NUM_STARS);
}

StarfieldScreensaver::~StarfieldScreensaver() {
    // ScreensaverManager owns these in a unique_ptr and does not stop the active
    // one before destroying it, so a screensaver torn down while running would
    // otherwise leave frame_timer_cb armed on a freed `this`. lv_timer_cancel_safe()
    // self-guards on lv_is_initialized() and neuters rather than unlinking, which
    // is what makes it safe from a destructor and after lv_deinit has already
    // reclaimed the timer (#750, #751, #1173).
    cancel_timer();
}

void StarfieldScreensaver::cancel_timer() {
    if (timer_) {
        helix::ui::lv_timer_cancel_safe(timer_);
        timer_ = nullptr;
    }
}

void StarfieldScreensaver::stop() {
    if (!active_) {
        return;
    }

    spdlog::info("[Screensaver] Stopping starfield");

    cancel_timer();

    // The overlay is deleted on a later timer pass. The canvas is hidden first, so
    // no refresh or snapshot before then draws from the buffer freed here.
    if (canvas_) {
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    if (draw_buf_) {
        lv_free(draw_buf_);
        draw_buf_ = nullptr;
    }

    if (overlay_) {
        helix::ui::safe_delete_deferred(overlay_);
        canvas_ = nullptr; // deleted as child of overlay
    }

    sim_.stars().clear();
    active_ = false;
}

void StarfieldScreensaver::frame_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<StarfieldScreensaver*>(lv_timer_get_user_data(timer));
    if (!self || !self->active_)
        return;
    self->render_frame(self->clock_.advance(lv_tick_get()));
}

void StarfieldScreensaver::render_frame(uint32_t dt_ms) {
    if (!canvas_ || !draw_buf_)
        return;
    FrameTarget target{draw_buf_, draw_buf_stride_, static_cast<uint32_t>(screen_w_),
                       static_cast<uint32_t>(screen_h_)};
    sim_.step(dt_ms, target, rng_);
    // The whole canvas, not the box the step changed. On a double-buffered display a partial
    // invalidation makes LVGL copy the previous frame's area minus this one into the other
    // buffer, in strips, every frame; a full-canvas one leaves it nothing to sync.
    lv_obj_invalidate(canvas_);
}

#endif // HELIX_ENABLE_SCREENSAVER
