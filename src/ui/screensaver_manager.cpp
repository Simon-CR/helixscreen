// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef HELIX_ENABLE_SCREENSAVER

#include "ui_screensaver.h"

#include "display_settings_manager.h"
#include "lvgl/src/misc/lv_timer_private.h" // lv_timer_t::period; LVGL has no period getter
#include "refresh_period_hold.h"
#include "screen_hide_hold.h"
#include "screensaver.h"
#include "screensaver_pipes.h"
#include "screensaver_starfield.h"

#include <spdlog/spdlog.h>

uint32_t helix::ui::screensaver_timer_period_ms() {
    lv_display_t* disp = lv_display_get_default();
    const lv_timer_t* refr = disp ? lv_display_get_refr_timer(disp) : nullptr;
    return refr ? refr->period : LV_DEF_REFR_PERIOD;
}

ScreensaverManager& ScreensaverManager::instance() {
    static ScreensaverManager mgr;
    return mgr;
}

ScreensaverManager::ScreensaverManager() {
    // Register all screensaver implementations
    screensavers_.push_back(std::make_unique<FlyingToasterScreensaver>());
    screensavers_.push_back(std::make_unique<StarfieldScreensaver>());
    screensavers_.push_back(std::make_unique<PipesScreensaver>());
}

void ScreensaverManager::start(ScreensaverType type) {
    if (type == ScreensaverType::OFF) {
        stop();
        return;
    }

    // Stop current screensaver if different type is requested. The screen hold stays
    // out until the new saver has started or failed, so the panel is never uncovered
    // between the two.
    if (active_ && active_->type() != type) {
        active_->stop();
        active_ = nullptr;
    }

    // Already running the requested type
    if (active_ && active_->is_active()) {
        return;
    }

    Screensaver* ss = find(type);
    if (!ss) {
        spdlog::warn("[ScreensaverManager] No screensaver registered for type {}",
                     static_cast<int>(type));
        release_screen();
        release_refresh_period();
        return;
    }

    // The saver reads the refresh period as it starts, so the hold goes out first.
    hold_refresh_period();
    ss->start();
    if (!ss->is_active()) {
        spdlog::warn("[ScreensaverManager] Screensaver type {} did not start",
                     static_cast<int>(type));
        active_ = nullptr;
        release_screen();
        release_refresh_period();
        return;
    }

    active_ = ss;
    hold_screen();
    spdlog::info("[ScreensaverManager] Started screensaver type {}", static_cast<int>(type));
}

void ScreensaverManager::stop() {
    if (active_) {
        active_->stop();
        spdlog::info("[ScreensaverManager] Stopped screensaver type {}",
                     static_cast<int>(active_->type()));
        active_ = nullptr;
    }
    release_screen();
    release_refresh_period();
}

void ScreensaverManager::hold_screen() {
    if (!holds_screen_) {
        helix::active_screen_hide_hold().acquire(lv_screen_active());
        holds_screen_ = true;
    }
}

void ScreensaverManager::release_screen() {
    if (holds_screen_) {
        holds_screen_ = false;
        helix::active_screen_hide_hold().release();
    }
}

void ScreensaverManager::hold_refresh_period() {
    if (!holds_refresh_period_) {
        helix::active_refresh_period_hold().acquire();
        holds_refresh_period_ = true;
    }
}

void ScreensaverManager::release_refresh_period() {
    if (holds_refresh_period_) {
        holds_refresh_period_ = false;
        helix::active_refresh_period_hold().release();
    }
}

bool ScreensaverManager::is_active() const {
    return active_ && active_->is_active();
}

ScreensaverType ScreensaverManager::configured_type() {
    int type_int = helix::DisplaySettingsManager::instance().get_screensaver_type();
    if (type_int < 0 || type_int > 3) {
        return ScreensaverType::OFF;
    }
    return static_cast<ScreensaverType>(type_int);
}

Screensaver* ScreensaverManager::find(ScreensaverType type) const {
    for (auto& ss : screensavers_) {
        if (ss->type() == type) {
            return ss.get();
        }
    }
    return nullptr;
}

#endif // HELIX_ENABLE_SCREENSAVER
