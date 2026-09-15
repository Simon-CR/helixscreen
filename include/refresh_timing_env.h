// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "refresh_timing.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>

// Header-only: the DRM backend reads the HELIX_EGL_* switches from here, and that backend is
// also linked into binaries that do not carry DisplayManager.

namespace helix {

namespace refresh_timing_detail {

/// Whole milliseconds in [min_ms, max_ms]. An unset variable reads as nullopt; a set one
/// that is not a plain decimal number in range reads as nullopt with a warning.
inline std::optional<uint32_t> ms_from_env(const char* name, uint32_t min_ms, uint32_t max_ms) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    // strtoul skips leading whitespace and accepts a sign, so the first character is
    // checked here: only a digit starts a value.
    bool ok = *value >= '0' && *value <= '9';
    unsigned long parsed = 0;
    if (ok) {
        errno = 0;
        char* end = nullptr;
        parsed = std::strtoul(value, &end, 10);
        ok = errno == 0 && *end == '\0' && parsed >= min_ms && parsed <= max_ms;
    }
    if (!ok) {
        spdlog::warn("[RefreshTiming] Ignoring {}='{}': expected whole milliseconds, {} to {}",
                     name, value, min_ms, max_ms);
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

inline bool scope_all_from_env() {
    const char* value = std::getenv("HELIX_REFR_PERIOD_SCOPE");
    if (value == nullptr || std::strcmp(value, "display") == 0) {
        return false;
    }
    if (std::strcmp(value, "all") == 0) {
        return true;
    }
    spdlog::warn("[RefreshTiming] Ignoring HELIX_REFR_PERIOD_SCOPE='{}': expected display or all",
                 value);
    return false;
}

} // namespace refresh_timing_detail

/// Reads HELIX_REFR_PERIOD_MS, HELIX_REFR_PERIOD_SCOPE, HELIX_SCREENSAVER_REFR_PERIOD_MS and
/// HELIX_LOOP_MIN_SLEEP_MS. A rejected value keeps that field's default.
inline RefreshTiming refresh_timing_from_env() {
    using refresh_timing_detail::ms_from_env;
    RefreshTiming t;
    t.refr_period_ms = ms_from_env("HELIX_REFR_PERIOD_MS", RefreshTiming::MIN_PERIOD_MS,
                                   RefreshTiming::MAX_PERIOD_MS)
                           .value_or(0);
    t.scope_all = refresh_timing_detail::scope_all_from_env();
    t.screensaver_refr_period_ms =
        ms_from_env("HELIX_SCREENSAVER_REFR_PERIOD_MS", RefreshTiming::MIN_PERIOD_MS,
                    RefreshTiming::MAX_PERIOD_MS)
            .value_or(0);
    t.loop_min_sleep_ms = ms_from_env("HELIX_LOOP_MIN_SLEEP_MS", RefreshTiming::MIN_LOOP_SLEEP_MS,
                                      RefreshTiming::MAX_LOOP_SLEEP_MS)
                              .value_or(RefreshTiming::DEFAULT_LOOP_MIN_SLEEP_MS);
    return t;
}

/// An EGL presentation switch: `1` is on, `0` or unset is off, anything else is off with a
/// warning.
inline bool egl_switch_from_env(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || std::strcmp(value, "0") == 0) {
        return false;
    }
    if (std::strcmp(value, "1") == 0) {
        return true;
    }
    spdlog::warn("[RefreshTiming] Ignoring {}='{}': expected 0 or 1", name, value);
    return false;
}

/// HELIX_EGL_VSYNC: wait for each page flip.
inline bool egl_vsync_from_env() {
    return egl_switch_from_env("HELIX_EGL_VSYNC");
}

/// HELIX_EGL_PARTIAL_UPLOAD: send the display texture only the areas LVGL flushed.
inline bool egl_partial_upload_from_env() {
    return egl_switch_from_env("HELIX_EGL_PARTIAL_UPLOAD");
}

/// HELIX_EGL_XRGB: keep the display XRGB8888 and present it with the X byte ignored.
inline bool egl_xrgb_from_env() {
    return egl_switch_from_env("HELIX_EGL_XRGB");
}

/// What became of an EGL switch the environment may have asked for.
enum class EglSwitch {
    Off,      ///< not asked for; the driver keeps its default
    On,       ///< asked for, and the driver took it
    Declined, ///< asked for, and the driver refused it
};

/// Calls `turn_on` when the switch `name` asks for it; `turn_on` returns whether the driver
/// took the setting.
inline EglSwitch apply_egl_switch_from_env(const char* name, const std::function<bool()>& turn_on) {
    if (!egl_switch_from_env(name)) {
        return EglSwitch::Off;
    }
    return turn_on() ? EglSwitch::On : EglSwitch::Declined;
}

/// Calls `set_vsync(true)` when HELIX_EGL_VSYNC asks for it, and reports whether it did.
/// Otherwise the driver keeps its own default.
inline bool apply_egl_vsync_from_env(const std::function<void(bool)>& set_vsync) {
    return apply_egl_switch_from_env("HELIX_EGL_VSYNC", [&set_vsync] {
               set_vsync(true);
               return true;
           }) == EglSwitch::On;
}

} // namespace helix
