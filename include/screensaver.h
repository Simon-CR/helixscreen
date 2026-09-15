// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include <cstdint>
#include <lvgl.h>
#include <memory>
#include <vector>

/**
 * @brief Available screensaver types
 *
 * Values map directly to the settings dropdown index and persisted config value.
 */
enum class ScreensaverType : int {
    OFF = 0,
    FLYING_TOASTERS = 1,
    STARFIELD = 2,
    PIPES_3D = 3,
};

namespace helix::ui {

/**
 * @brief Color format of the starfield canvas
 *
 * Opaque, so the full-screen canvas draws as a straight copy instead of an alpha blend.
 * Production 32 bpp displays run ARGB8888 and copy an XRGB8888 image byte for byte, so
 * every canvas pixel's X byte must be 0xFF or that pixel draws transparent ("The alpha
 * trap" in docs/devel/GPU_ACCELERATION.md). The starfield writes every pixel itself with
 * that byte set. A canvas drawn with lv_draw_* cannot rely on it: see PIPES_CANVAS_FORMAT.
 */
inline constexpr lv_color_format_t SCREENSAVER_CANVAS_FORMAT = LV_COLOR_FORMAT_XRGB8888;

/**
 * @brief Color format of the pipes canvas
 *
 * Pipes draws antialiased segments and joints with lv_draw_*. On aarch64, LVGL's NEON
 * blends into an XRGB8888 destination write 0 into each pixel's 4th byte, and an XRGB8888
 * canvas is copied byte for byte into the ARGB8888 display, so those pixels would draw
 * transparent. An ARGB8888 canvas keeps real alpha, and it is filled opaque before anything
 * is drawn on it, so it still covers every pixel under the transparent overlay.
 */
inline constexpr lv_color_format_t PIPES_CANVAS_FORMAT = LV_COLOR_FORMAT_ARGB8888;

/**
 * @brief Row pitch in bytes lv_canvas_set_buffer() uses for a w-wide canvas in format cf
 *
 * lv_canvas_set_buffer() does not take the caller's word for packing: it
 * derives an LV_DRAW_BUF_STRIDE_ALIGN-rounded stride and sizes the canvas
 * extent from it, so a buffer allocated at w * h * bytes-per-pixel under-runs
 * the extent whenever the stride exceeds w * bytes-per-pixel
 * (prestonbrown/helixscreen#1591). The canvas screensavers size their
 * allocations and direct pixel writes from this pitch so they cannot disagree
 * with what LVGL steps rows by.
 */
inline uint32_t screensaver_canvas_stride_bytes(int32_t w, lv_color_format_t cf) {
    return lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf);
}

/**
 * @brief Period for a screensaver's frame timer: the default display's refresh period
 *
 * A saver ticking faster than the display refreshes computes frames nobody sees, and one
 * ticking slower moves in visible steps. LV_DEF_REFR_PERIOD when there is no display or
 * its refresh timer has been deleted.
 */
uint32_t screensaver_timer_period_ms();

} // namespace helix::ui

/**
 * @brief Abstract base class for all screensaver implementations
 *
 * Each screensaver owns its overlay and rendering resources.
 * The ScreensaverManager routes start/stop calls to the active instance.
 */
class Screensaver {
  public:
    virtual ~Screensaver() = default;

    /** @brief Create overlay and begin rendering */
    virtual void start() = 0;

    /** @brief Stop rendering and destroy overlay */
    virtual void stop() = 0;

    /** @brief Check if this screensaver is currently running */
    virtual bool is_active() const = 0;

    /** @brief Return the type identifier for this screensaver */
    virtual ScreensaverType type() const = 0;
};

namespace helix {
class ScreensaverManagerTestAccess;
}

/**
 * @brief Registry and router for screensaver instances
 *
 * Owns all screensaver implementations. Routes start/stop based on
 * the configured type from DisplaySettingsManager.
 */
class ScreensaverManager {
  public:
    /** @brief Must not be called before LVGL initialization */
    static ScreensaverManager& instance();

    ScreensaverManager(const ScreensaverManager&) = delete;
    ScreensaverManager& operator=(const ScreensaverManager&) = delete;

    /**
     * @brief Start the specified screensaver type
     *
     * Stops any currently active screensaver first. Does nothing if type is OFF.
     *
     * While a saver runs, the active screen is hidden beneath its overlay
     * (helix::ScreenHideHold), and it stays hidden across a switch between types.
     * For the same span the display refreshes at HELIX_SCREENSAVER_REFR_PERIOD_MS when
     * one is configured (helix::RefreshPeriodHold), set before the saver starts so the
     * saver's own timer follows it.
     * A saver that fails to start leaves the manager inactive, and the screen and
     * refresh period as they were before any saver ran.
     */
    void start(ScreensaverType type);

    /** @brief Stop whatever screensaver is currently active */
    void stop();

    /** @brief Check if any screensaver is currently active */
    bool is_active() const;

    /** @brief Read configured screensaver type from DisplaySettingsManager */
    static ScreensaverType configured_type();

  private:
    friend class helix::ScreensaverManagerTestAccess;

    ScreensaverManager();
    ~ScreensaverManager() = default;

    /** @brief Find screensaver instance by type, or nullptr */
    Screensaver* find(ScreensaverType type) const;

    /** @brief Take the active-screen hide hold, once however many savers run in turn */
    void hold_screen();

    /** @brief Give back the hold taken by hold_screen(), if one is out */
    void release_screen();

    /** @brief Take the refresh period hold, once however many savers run in turn */
    void hold_refresh_period();

    /** @brief Give back the hold taken by hold_refresh_period(), if one is out */
    void release_refresh_period();

    std::vector<std::unique_ptr<Screensaver>> screensavers_;
    Screensaver* active_ = nullptr;
    bool holds_screen_ = false;
    bool holds_refresh_period_ = false;
};

#endif // HELIX_ENABLE_SCREENSAVER
