// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_spoolman_manager.cpp
 * @brief Unit tests for SpoolmanManager singleton
 *
 * Tests refcount-based polling, circuit breaker state, and spoolman
 * availability gating. Does not require a real MoonrakerAPI — exercises
 * the internal state machine via the SpoolmanManagerTestAccess friend class.
 */

#include "ui_spoolman_overlay.h"
#include "ui_update_queue.h"

#include "../test_helpers/registered_backend.h"
#include "../test_helpers/seeded_override.h"
#include "../test_helpers/update_queue_test_access.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "spoolman_manager.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"

// ============================================================================
// TestAccess — friend class for private member inspection (L065: no test
// methods on the class itself)
// ============================================================================

class SpoolmanManagerTestAccess {
  public:
    static int poll_refcount(SpoolmanManager& m) {
        return m.poll_refcount_;
    }
    /// nullptr until something both wants polling and can be served.
    static lv_timer_t* poll_timer(SpoolmanManager& m) {
        return m.poll_timer_;
    }
    /// Bind the availability observer for this test; init_subjects() is
    /// idempotent, so a prior call would otherwise leave it unbound here.
    static void rewire_subjects(SpoolmanManager& m) {
        {
            std::lock_guard<std::recursive_mutex> lock(m.mutex_);
            m.print_state_observer_.reset();
            m.spoolman_availability_observer_.reset();
            m.initialized_ = false;
        }
        m.init_subjects();
    }
    static bool cb_open(SpoolmanManager& m) {
        return m.cb_open_;
    }
    static int consecutive_failures(SpoolmanManager& m) {
        return m.consecutive_failures_;
    }

    static void reset(SpoolmanManager& m) {
        // Delete any active timer to avoid leaks between tests
        if (m.poll_timer_ && lv_is_initialized()) {
            lv_timer_delete(m.poll_timer_);
            m.poll_timer_ = nullptr;
        }
        m.poll_refcount_ = 0;
        m.last_refresh_ms_ = 0;
        m.consecutive_failures_ = 0;
        m.cb_tripped_at_ms_ = 0;
        m.cb_open_ = false;
        m.unavailable_notified_ = false;
        m.api_ = nullptr;
    }

    static void set_consecutive_failures(SpoolmanManager& m, int count) {
        m.consecutive_failures_ = count;
    }

    static void set_cb_open(SpoolmanManager& m, bool open) {
        m.cb_open_ = open;
        if (open) {
            m.cb_tripped_at_ms_ = lv_tick_get();
        }
    }

    /// refresh_spoolman_weights() debounces itself; a case that fetches twice
    /// has to step past it.
    static void clear_debounce(SpoolmanManager& m) {
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.last_refresh_ms_ = 0;
    }

    /// An id another case left unresolvable is never fetched, and a shutdown
    /// flag another file's teardown latched no-ops every queued answer.
    static void reset_identity(SpoolmanManager& m) {
        SpoolmanManager::s_shutdown_flag.store(false, std::memory_order_release);
        std::lock_guard<std::recursive_mutex> lock(m.mutex_);
        m.identity_cache_.clear();
        m.identity_unresolvable_.clear();
    }
};

using TA = SpoolmanManagerTestAccess;

// ============================================================================
// LVGL Init (once per translation unit, idempotent)
// ============================================================================

namespace {
struct LVGLInitializerSpoolman {
    LVGLInitializerSpoolman() {
        static bool initialized = false;
        if (!initialized) {
            lv_init_safe();
            lv_display_t* disp = lv_display_create(800, 480);
            alignas(64) static lv_color_t buf[800 * 10];
            lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
            initialized = true;
        }
    }
};
static LVGLInitializerSpoolman lvgl_init;
} // namespace

// ============================================================================
// Fixture — reset singleton state between tests (L053)
// ============================================================================

struct SpoolmanFixture {
    static bool queue_initialized;

    SpoolmanFixture() {
        if (!queue_initialized) {
            helix::ui::update_queue_init();
            queue_initialized = true;
        }
        TA::reset(SpoolmanManager::instance());
        get_printer_state().init_subjects(false);
    }

    ~SpoolmanFixture() {
        TA::reset(SpoolmanManager::instance());
    }

    /// Set spoolman availability and drain the update queue so the subject
    /// value is visible synchronously (set_spoolman_available uses queue_update).
    void set_spoolman_available(bool available) {
        get_printer_state().set_spoolman_available(available);
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }
};

bool SpoolmanFixture::queue_initialized = false;

// ============================================================================
// Polling Refcount Tests
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: start increments refcount", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    REQUIRE(TA::poll_refcount(mgr) == 0);

    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 2);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: stop decrements refcount", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 2);

    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: multiple starts and stops balance",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();
    mgr.start_spoolman_polling();

    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();

    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: stop below zero clamps at 0", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // Stop without any prior start
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);

    // Multiple excess stops
    mgr.stop_spoolman_polling();
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: start after full stop restarts cleanly",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    mgr.start_spoolman_polling();
    mgr.stop_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 0);

    // Restart — refcount goes from 0 back to 1
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);
}

// ============================================================================
// Circuit Breaker Tests
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: reset clears all circuit breaker state",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // Dirty up the state
    TA::set_consecutive_failures(mgr, 5);
    TA::set_cb_open(mgr, true);

    REQUIRE(TA::cb_open(mgr) == true);
    REQUIRE(TA::consecutive_failures(mgr) == 5);

    // Full reset
    TA::reset(mgr);

    REQUIRE(TA::cb_open(mgr) == false);
    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_api resets circuit breaker", "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    TA::set_consecutive_failures(mgr, 3);
    TA::set_cb_open(mgr, true);

    // set_api(nullptr) calls reset_circuit_breaker internally
    mgr.set_api(nullptr);

    REQUIRE(TA::consecutive_failures(mgr) == 0);
    REQUIRE(TA::cb_open(mgr) == false);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_consecutive_failures updates count",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    TA::set_consecutive_failures(mgr, 3);
    REQUIRE(TA::consecutive_failures(mgr) == 3);

    TA::set_consecutive_failures(mgr, 0);
    REQUIRE(TA::consecutive_failures(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanManager: set_cb_open toggles circuit breaker",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    REQUIRE(TA::cb_open(mgr) == false);

    TA::set_cb_open(mgr, true);
    REQUIRE(TA::cb_open(mgr) == true);

    TA::set_cb_open(mgr, false);
    REQUIRE(TA::cb_open(mgr) == false);
}

// ============================================================================
// Spoolman Availability Gating
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanManager: start_polling defers until spoolman is available",
                 "[spoolman]") {
    // Wanting to poll and being able to poll arrive in either order, and at boot
    // it is always want-first: panels activate synchronously inside init_ui()
    // while set_spoolman_available() is still sitting in the UpdateQueue. The
    // request is therefore recorded and acted on later, never discarded.
    auto& mgr = SpoolmanManager::instance();
    TA::rewire_subjects(mgr);

    SECTION("a start while unavailable is remembered, not dropped") {
        set_spoolman_available(false);

        mgr.start_spoolman_polling();
        CHECK(TA::poll_refcount(mgr) == 1);    // the wish survives
        CHECK(TA::poll_timer(mgr) == nullptr); // but nothing polls yet
    }

    SECTION("start polls immediately when spoolman is already available") {
        set_spoolman_available(true);

        mgr.start_spoolman_polling();
        CHECK(TA::poll_refcount(mgr) == 1);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }

    SECTION("availability arriving later arms the deferred request on its own") {
        set_spoolman_available(false);
        mgr.start_spoolman_polling();
        REQUIRE(TA::poll_timer(mgr) == nullptr);

        // No second start_spoolman_polling() here on purpose: in production
        // nothing makes that call, which is why the poll never armed at boot.
        set_spoolman_available(true);

        CHECK(TA::poll_refcount(mgr) == 1);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }

    SECTION("losing spoolman stops the timer but keeps the request") {
        set_spoolman_available(true);
        mgr.start_spoolman_polling();
        REQUIRE(TA::poll_timer(mgr) != nullptr);

        set_spoolman_available(false);
        CHECK(TA::poll_timer(mgr) == nullptr);
        // The panel is still up and still wants polling, so a Spoolman that
        // comes back must resume without it having to ask again.
        CHECK(TA::poll_refcount(mgr) == 1);

        set_spoolman_available(true);
        CHECK(TA::poll_timer(mgr) != nullptr);
    }
}

// ============================================================================
// refresh without API
// ============================================================================

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanManager: refresh_spoolman_weights returns early without API",
                 "[spoolman]") {
    auto& mgr = SpoolmanManager::instance();

    // No API set — should return without crash
    REQUIRE_NOTHROW(mgr.refresh_spoolman_weights());
}

// ============================================================================
// SpoolmanOverlay poll-reference discipline (#1159)
// ============================================================================
//
// The overlay applies the persisted sync_enabled setting on every open, and the
// apply took an unmatched poll reference each time — refcount climbed forever and
// the poll timer could never be deleted. These tests pin the ownership contract:
// at most one reference per overlay instance, given back on dismissal.
//
// apply_sync() itself is a lambda inside the async load_from_database() chain, so
// the tests drive set_poll_ref() — the single helper that lambda now calls — plus
// the real public on_deactivate().

class SpoolmanOverlayTestAccess {
  public:
    /// Stand-in for load_from_database()'s apply_sync(enabled)
    static void apply_sync(helix::ui::SpoolmanOverlay& o, bool enabled) {
        o.set_poll_ref(enabled);
    }
    static bool holds_poll_ref(const helix::ui::SpoolmanOverlay& o) {
        return o.holds_poll_ref_;
    }
};

using OverlayTA = SpoolmanOverlayTestAccess;

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanOverlay: repeated opens do not leak poll references",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    helix::ui::SpoolmanOverlay overlay;

    // Open #1 — sync enabled, one reference taken
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);
    REQUIRE(OverlayTA::holds_poll_ref(overlay));

    // Dismissal gives it back
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
    REQUIRE_FALSE(OverlayTA::holds_poll_ref(overlay));

    // Open #2 — must not stack a second reference
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture, "SpoolmanOverlay: repeated sync apply takes one reference",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    helix::ui::SpoolmanOverlay overlay;

    // load_from_database()'s key-fallback chain can apply the value more than
    // once per open (new key -> legacy key -> default)
    OverlayTA::apply_sync(overlay, true);
    OverlayTA::apply_sync(overlay, true);
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 0);
}

TEST_CASE_METHOD(SpoolmanFixture,
                 "SpoolmanOverlay: release never steals another holder's reference",
                 "[spoolman][overlay]") {
    auto& mgr = SpoolmanManager::instance();
    set_spoolman_available(true);

    // A panel (HomePanel/AmsPanel/SpoolmanPanel) holds its own reference
    mgr.start_spoolman_polling();
    REQUIRE(TA::poll_refcount(mgr) == 1);

    helix::ui::SpoolmanOverlay overlay;

    // Overlay opened with sync disabled — it never took a reference, so applying
    // "disabled" and dismissing must leave the panel's reference alone
    OverlayTA::apply_sync(overlay, false);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 1);

    // Same for a dismissal that follows a toggle-off
    OverlayTA::apply_sync(overlay, true);
    REQUIRE(TA::poll_refcount(mgr) == 2);
    OverlayTA::apply_sync(overlay, false);
    REQUIRE(TA::poll_refcount(mgr) == 1);
    overlay.on_deactivate(DeactivateReason::NavigateAway);
    REQUIRE(TA::poll_refcount(mgr) == 1);
}

// ============================================================================
// The lane's Spoolman record follows every fetch (prestonbrown/helixscreen#1653)
// ============================================================================

using helix::AmsBackend;
using helix::AmsBackendMock;
using helix::AmsState;
using helix::SlotInfo;

namespace {

/// A backend that keeps its own remaining weight, the way AFC reads one off
/// its status payload.
class LocalWeightBackend : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;

    [[nodiscard]] bool tracks_weight_locally() const override {
        return true;
    }
};

} // namespace

/// A mock Moonraker behind the manager, and an identity cache no earlier case
/// has touched.
struct SpoolmanLaneFixture : SpoolmanFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;

    SpoolmanLaneFixture() : api(client, get_printer_state()) {
        // A fetch's answer bumps AmsState's slots_version, which needs the
        // subject to exist.
        AmsState::instance().init_subjects(true);
        TA::reset_identity(SpoolmanManager::instance());
        set_spoolman_available(true);
        SpoolmanManager::instance().set_api(&api);
    }

    ~SpoolmanLaneFixture() {
        SpoolmanManager::instance().set_api(nullptr);
        drain();
        TA::reset_identity(SpoolmanManager::instance());
    }

    /// What the server holds for spool @p id. A case states every field it
    /// asserts on here rather than resting on the mock's seed inventory.
    SpoolInfo& server_spool(int id) {
        auto& spools = api.spoolman_mock().get_mock_spools();
        auto it = std::find_if(spools.begin(), spools.end(),
                               [id](const SpoolInfo& s) { return s.id == id; });
        REQUIRE(it != spools.end());
        return *it;
    }

    void remove_server_spool(int id) {
        auto& spools = api.spoolman_mock().get_mock_spools();
        spools.erase(std::remove_if(spools.begin(), spools.end(),
                                    [id](const SpoolInfo& s) { return s.id == id; }),
                     spools.end());
    }

    /// Fetch every linked slot. The mock answers inside the call, and the
    /// answer reaches a lane only when the update queue drains.
    static void fetch() {
        TA::clear_debounce(SpoolmanManager::instance());
        SpoolmanManager::instance().refresh_spoolman_weights();
    }

    static void drain() {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    }

    static void poll() {
        fetch();
        drain();
    }

    static void link(AmsBackend& backend, int slot, int spool_id) {
        SlotInfo info = backend.get_slot_info(slot);
        info.spoolman_id = spool_id;
        REQUIRE(backend.set_slot_info(slot, info, /*persist=*/false).success());
    }
};

namespace {

void state_polymaker_pla(SpoolInfo& spool) {
    spool.vendor = "Polymaker";
    spool.vendor_id = 7;
    spool.material = "PLA";
    spool.filament_name = "PolyTerra Charcoal";
    spool.color_hex = "1A1A2E";
    spool.initial_weight_g = 1000.0;
    spool.remaining_weight_g = 850.0;
}

} // namespace

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a fetch files the spool as the lane's Spoolman record",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    poll();

    const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->spoolman_id == 1);
    CHECK(record->spoolman_vendor_id == 7);
    CHECK(record->brand == "Polymaker");
    CHECK(record->material == "PLA");
    CHECK(record->spool_name == "PolyTerra Charcoal");
    CHECK(record->color_rgb == 0x1A1A2EU);
    CHECK(record->total_weight_g == 1000.0F);
    CHECK(record->remaining_weight_g == 850.0F);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool edited on the server reaches its lane on the next fetch",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    poll();
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
    // The id is already known, so nothing about this spool is new to the
    // identity cache when the edited record arrives.
    REQUIRE(SpoolmanManager::find_identity(1).has_value());

    server_spool(1).material = "PETG";
    server_spool(1).color_hex = "FF5500";
    poll();

    const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
    REQUIRE(record.has_value());
    CHECK(record->material == "PETG");
    CHECK(record->color_rgb == 0xFF5500U);
}

TEST_CASE_METHOD(
    SpoolmanLaneFixture,
    "SpoolmanManager: a slot re-bound while its fetch was in flight takes nothing from it",
    "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    link(*backend, 1, 2);
    state_polymaker_pla(server_spool(1));
    state_polymaker_pla(server_spool(2));

    // The mock answers inside the fetch and the answer waits in the update
    // queue, which is the window a real round trip leaves open.
    fetch();
    link(*backend, 0, 3);
    drain();

    // Both answers ran: slot 1's was filed, and slot 0's reached the identity
    // cache, which it fills ahead of the binding check.
    REQUIRE(helix::ams::lane_sources(backend.lane(1)).spoolman.has_value());
    REQUIRE(SpoolmanManager::find_identity(1).has_value());
    CHECK_FALSE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
}

TEST_CASE_METHOD(SpoolmanLaneFixture, "SpoolmanManager: the weights a fetch files on a lane",
                 "[spoolman][lane][1653]") {
    SECTION("a backend that keeps its own remaining weight gets only Spoolman's total") {
        helix::test::RegisteredBackend<LocalWeightBackend> backend(2);
        link(*backend, 0, 1);
        state_polymaker_pla(server_spool(1));

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->total_weight_g == 1000.0F);
        CHECK_FALSE(record->remaining_weight_g.has_value());
    }

    SECTION("a spool Spoolman holds no weight for states no weight") {
        // Spoolman serves both weights as null when neither the spool nor its
        // filament has one, and the parser reads null as zero.
        helix::test::RegisteredBackend<AmsBackendMock> backend(2);
        link(*backend, 0, 1);
        SpoolInfo& spool = server_spool(1);
        state_polymaker_pla(spool);
        spool.initial_weight_g = 0.0;
        spool.remaining_weight_g = 0.0;

        poll();

        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Polymaker");
        CHECK_FALSE(record->total_weight_g.has_value());
        CHECK_FALSE(record->remaining_weight_g.has_value());
    }
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a linked lane's catalog pick survives a fetch of its spool",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    state_polymaker_pla(server_spool(1));

    // What a backend's start reloads: the identity the spool had when it was
    // last fetched, beside the product the person picked for it.
    helix::ams::FilamentSlotOverride stored;
    stored.spoolman_id = 1;
    stored.brand = "Stored Brand";
    stored.material = "PETG";
    stored.color_rgb = 0xFF0000;
    stored.color_set = true;
    stored.catalog_id = "polymaker-polyterra-pla-charcoal";
    stored.product_name = "PolyTerra PLA Charcoal";
    helix::test::file_override_as_lane_records(*backend, 0, stored);

    SpoolmanManager::file_spool_on_lane(backend.lane(0), server_spool(1),
                                        backend->tracks_weight_locally());

    const auto shown = helix::ams::resolve(helix::ams::lane_sources(backend.lane(0)));
    CHECK(shown.catalog_id == "polymaker-polyterra-pla-charcoal");
    CHECK(shown.product_name == "PolyTerra PLA Charcoal");
    CHECK(shown.brand == "Polymaker");
    CHECK(shown.material == "PLA");
    CHECK(shown.color_rgb == 0x1A1A2EU);
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a field the server stops stating stops outranking the lane's "
                 "other sources",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    SpoolInfo& spool = server_spool(1);
    state_polymaker_pla(spool);

    helix::ams::Observation firmware(helix::ams::ObservationSource::VendorCache);
    firmware.brand = "Firmware Brand";
    firmware.spool_name = "Firmware Name";
    helix::ams::ingest(backend.lane(0), firmware);

    poll();
    REQUIRE(helix::ams::resolve(helix::ams::lane_sources(backend.lane(0))).brand == "Polymaker");

    // The vendor is removed on the server and the filament loses its name.
    // An empty field is Spoolman saying nothing about it, not a blank brand.
    spool.vendor.clear();
    spool.vendor_id = 0;
    spool.filament_name.clear();
    poll();

    const auto sources = helix::ams::lane_sources(backend.lane(0));
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->material == "PLA");
    CHECK_FALSE(sources.spoolman->brand.has_value());
    CHECK_FALSE(sources.spoolman->spoolman_vendor_id.has_value());
    CHECK_FALSE(sources.spoolman->spool_name.has_value());

    const auto shown = helix::ams::resolve(sources);
    CHECK(shown.brand == "Firmware Brand");
    CHECK(shown.spool_name == "Firmware Name");
}

TEST_CASE_METHOD(SpoolmanLaneFixture,
                 "SpoolmanManager: a spool Spoolman denies loses its cached lane record; an "
                 "unreachable Spoolman does not",
                 "[spoolman][lane][1653]") {
    helix::test::RegisteredBackend<AmsBackendMock> backend(2);
    link(*backend, 0, 1);
    link(*backend, 1, 2);

    // The record a backend's start filed from its stored override.
    helix::ams::Observation cached(helix::ams::ObservationSource::Spoolman);
    cached.spoolman_id = 1;
    cached.brand = "Cached Brand";
    helix::ams::ingest(backend.lane(0), cached);
    REQUIRE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());

    SECTION("not found drops it") {
        remove_server_spool(1);
        poll();

        REQUIRE(SpoolmanManager::is_identity_unresolvable(1));
        CHECK_FALSE(helix::ams::lane_sources(backend.lane(0)).spoolman.has_value());
    }

    SECTION("not found for a slot re-bound meanwhile leaves it alone") {
        remove_server_spool(1);
        fetch();
        link(*backend, 0, 3);
        drain();

        REQUIRE(SpoolmanManager::is_identity_unresolvable(1));
        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Cached Brand");
    }

    SECTION("an unreachable Spoolman leaves it standing") {
        api.spoolman_mock().set_mock_spoolman_enabled(false);
        poll();

        // Both linked slots' fetches failed, and each failure was counted.
        REQUIRE(TA::consecutive_failures(SpoolmanManager::instance()) == 2);
        const auto record = helix::ams::lane_sources(backend.lane(0)).spoolman;
        REQUIRE(record.has_value());
        CHECK(record->brand == "Cached Brand");
    }
}
