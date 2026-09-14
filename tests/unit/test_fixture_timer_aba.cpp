// Copyright (C) 2025-2026 356C LLC
// TEST_MIRROR_OK: tests lv_timer_handler_safe() itself, test infrastructure by definition
// SPDX-License-Identifier: GPL-3.0-or-later
//
// lv_async_call's own timer wrapper (lib/lvgl/src/misc/lv_async.c#lv_async_timer_cb)
// deletes its lv_timer_t node BEFORE invoking the caller's function, so a
// second lv_async_call made from inside that function can allocate its new
// timer node at the address the first one just vacated (every lv_timer_t
// comes from the same fixed-size free list, lib/lvgl/src/misc/lv_ll.c). If
// lv_timer_handler_safe() re-finds a spent one-shot by pointer afterward, it
// deletes whatever now sits at that address instead of noticing its target is
// already gone, silently dropping the nested call.
//
// Mutation check: restore the pointer re-scan this test was written against
// (re-find `t` in the timer list and delete it whenever the loop decremented
// its repeat_count to zero, instead of gating on
// LV_GLOBAL_DEFAULT()->timer_state.timer_deleted) and this test goes red.

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "lvgl/lvgl.h"

#include <algorithm>
#include <vector>

#include "../catch_amalgamated.hpp"

namespace {

struct AbaProbe {
    lv_timer_t* outer_addr = nullptr;
    bool address_reused = false;
    bool inner_ran = false;
};

void inner_cb(void* user_data) {
    static_cast<AbaProbe*>(user_data)->inner_ran = true;
}

void outer_cb(void* user_data) {
    auto* probe = static_cast<AbaProbe*>(user_data);
    // By the time this runs, lv_async_timer_cb has already deleted the timer
    // node that carried this call — see the file comment above. Scheduling
    // the nested call here is what lets its allocation land on that freed
    // slot before lv_timer_handler_safe() gets a chance to look again.
    lv_async_call(inner_cb, probe);
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (t == probe->outer_addr) {
            probe->address_reused = true;
            break;
        }
    }
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "lv_timer_handler_safe delivers a nested lv_async_call after its timer's "
                 "address is reused (ABA)",
                 "[core][fixture][timer]") {
    AbaProbe probe;

    std::vector<lv_timer_t*> before;
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        before.push_back(t);
    }

    lv_async_call(outer_cb, &probe);

    // The one timer address that appeared as a side effect of the call above
    // is the node lv_async_call just created for it.
    for (lv_timer_t* t = lv_timer_get_next(nullptr); t != nullptr; t = lv_timer_get_next(t)) {
        if (std::find(before.begin(), before.end(), t) == before.end()) {
            probe.outer_addr = t;
            break;
        }
    }
    REQUIRE(probe.outer_addr != nullptr);

    lv_timer_handler_safe();

    // Confirms the ABA precondition this test relies on actually occurred —
    // if a future LVGL allocator change stopped reusing the address, this
    // fails here with a clear reason instead of the assertion below passing
    // for the wrong one.
    REQUIRE(probe.address_reused);

    CHECK(probe.inner_ran);
}
