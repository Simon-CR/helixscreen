// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef HELIX_ENABLE_SCREENSAVER

#include "screensaver.h"
#include "screensaver_motion.h"

#include <cstdint>
#include <lvgl.h>
#include <optional>
#include <random>
#include <vector>

/**
 * @brief Windows-style 3D Pipes screensaver
 *
 * Pipes grow through a 3D grid rendered with perspective projection.
 * Multiple pipes grow simultaneously, each with a different color.
 * Ball joints appear at direction changes. Camera angle randomizes on reset.
 * Ported from https://github.com/1j01/pipes
 */
class PipesScreensaver : public Screensaver {
  public:
    PipesScreensaver() = default;
    ~PipesScreensaver() override;

    void start() override;
    void stop() override;
    bool is_active() const override {
        return active_;
    }
    ScreensaverType type() const override {
        return ScreensaverType::PIPES_3D;
    }

  private:
    // Test-only seam: reads the draw buffer, timer and simulation state, and fixes
    // the seed. See tests/test_helpers/screensaver_test_access.h.
    friend class PipesScreensaverTestAccess;

    // Grid: 21x21x21 centered at origin (-10..+10), matching reference
    static constexpr int GRID_DIM = 21;
    static constexpr int GRID_OFFSET = 10;
    static constexpr int MAX_SEGMENTS = 500;
    static constexpr int MAX_ACTIVE_PIPES = 3;

    enum class Direction { POS_X = 0, NEG_X, POS_Y, NEG_Y, POS_Z, NEG_Z };

    struct GridPos {
        int x = 0, y = 0, z = 0;
    };

    struct ActivePipe {
        GridPos pos;
        Direction dir{};
        lv_color_t color{};
        lv_color_t shadow_color{};
        lv_color_t highlight_color{};
        int segment_count = 0;
        bool alive = false;
        bool has_prev_dir = false;
    };

    void reset_grid();
    void setup_camera();
    void start_new_pipe(ActivePipe& pipe);
    bool grow_pipe(ActivePipe& pipe, lv_layer_t* layer);
    bool project(float wx, float wy, float wz, int& sx, int& sy, float& depth) const;
    void draw_segment(lv_layer_t* layer, int sx1, int sy1, int sx2, int sy2, float depth,
                      const ActivePipe& pipe);
    void draw_joint(lv_layer_t* layer, int sx, int sy, float depth, const ActivePipe& pipe);
    /// Records a canvas area (canvas coordinates, inclusive) the open layer session draws into
    void mark_dirty(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    /// Finishes a layer session on the canvas, invalidating only the areas its draws marked
    void finish_layer(lv_layer_t* layer);
    GridPos next_pos(GridPos pos, Direction dir) const;
    bool in_bounds(GridPos pos) const;

    static void tick_timer_cb(lv_timer_t* timer);
    /// Grows the pipes by the steps `dt_ms` of frame time makes due
    void tick(uint32_t dt_ms);
    /// Grows every alive pipe one grid step, and starts a pipe when one has died
    void grow_step();

    /// Shared by stop() and the destructor — a timer cancelled only in stop()
    /// stays armed on a freed `this` on any teardown that skips it.
    void cancel_timer();

    bool active_ = false;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;

    // Draw buffer owned by the canvas, allocated at LVGL's row stride — see
    // screensaver_canvas_stride_bytes().
    uint8_t* draw_buf_ = nullptr;
    size_t draw_buf_size_ = 0;

    // Areas the open layer session draws into; finish_layer() invalidates them and clears it.
    std::vector<lv_area_t> dirty_areas_;

    // Grid occupancy
    bool grid_[GRID_DIM][GRID_DIM][GRID_DIM]{};

    // Multiple active pipes
    ActivePipe pipes_[MAX_ACTIVE_PIPES]{};
    int color_index_ = 0;
    int total_segments_ = 0;

    // Owned random sequence, seeded in start(); the camera, pipe starts and turns draw from it.
    std::minstd_rand rng_;
    // When set, start() seeds the random sequence from this instead of the clock, so a
    // run replays exactly.
    std::optional<uint32_t> fixed_seed_;
    helix::ui::screensaver::MotionClock clock_;
    helix::ui::screensaver::StepAccumulator steps_;

    // Perspective camera (precomputed basis vectors)
    float cam_pos_[3]{};
    float cam_right_[3]{};
    float cam_up_[3]{};
    float cam_fwd_[3]{};
    float focal_ = 0;

    int screen_w_ = 0;
    int screen_h_ = 0;
};

#endif // HELIX_ENABLE_SCREENSAVER
