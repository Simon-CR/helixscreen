// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <utility>

namespace helix::ui {

/// Run @p work on the next lv_timer_handler() pass, from a one-shot LVGL timer
/// (lv_async_call), outside any input dispatch: for structural UI changes an
/// event handler asks for, which must not run inside that event. Skipped when
/// @p token has expired by then, so an owner destroyed first cancels it.
///
/// Main thread only. AsyncLifetimeGuard::defer() posts to the UpdateQueue
/// instead; this runs from the LVGL timer list.
inline void run_next_tick(helix::LifetimeToken token, std::function<void()> work) {
    struct Ctx {
        helix::LifetimeToken tok;
        std::function<void()> work;
    };
    auto* ctx = new Ctx{std::move(token), std::move(work)};
    lv_async_call(
        [](void* data) {
            auto* c = static_cast<Ctx*>(data);
            if (c->tok.expired()) {
                delete c;
                return;
            }
            c->work();
            delete c;
        },
        ctx);
}

} // namespace helix::ui
