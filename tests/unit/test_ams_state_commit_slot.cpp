// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_ams_detail.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../ui_test_utils.h"
#include "ams_backend_afc.h"
#include "ams_backend_mock.h"
#include "ams_error.h"
#include "ams_state.h"
#include "ams_types.h"
#include "app_globals.h"
#include "display_numbering.h"
#include "filament_op_dispatch.h"
#include "lane_resolver.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "spoolman_manager.h"
#include "spoolman_types.h"
#include "test_helpers/afc_test_access.h"
#include "test_helpers/registered_backend.h"
#include "test_helpers/seeded_override.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// AFC-shaped mock: reports manages_active_spool()==true the way the real AFC
/// backend does. AmsBackendMock itself never does (it has no firmware behind
/// it), so the F2LNLQCC regression has to stage the capability here.
class ManagesActiveSpoolMock : public AmsBackendMock {
  public:
    using AmsBackendMock::AmsBackendMock;
    [[nodiscard]] bool manages_active_spool() const override {
        return true;
    }
};

/// A Spoolman record the identity cache will accept (mirrors the helper in
/// test_spoolman_identity_cache.cpp — needs a name to be cacheable).
SpoolInfo make_spool(int id, std::string vendor, std::string filament_name, std::string material) {
    SpoolInfo spool;
    spool.id = id;
    spool.vendor = std::move(vendor);
    spool.filament_name = std::move(filament_name);
    spool.material = std::move(material);
    spool.color_hex = "FFB6C1";
    spool.filament_id = 300 + id;
    spool.vendor_id = 400 + id;
    spool.remaining_weight_g = 850.0;
    spool.initial_weight_g = 1000.0;
    return spool;
}

/// CommitFixture registers its backend first, so it takes the first id block.
helix::ams::LaneId lane_of(int slot) {
    return helix::ams::lane_id_for(0, slot);
}

/// A stored record for a linked lane: the spool id and the identity that came
/// with the link.
helix::ams::FilamentSlotOverride linked_record(int spoolman_id) {
    helix::ams::FilamentSlotOverride record;
    record.spoolman_id = spoolman_id;
    record.brand = "Polymaker";
    record.material = "PLA";
    return record;
}

/// One AFC_stepper lane object, through the envelope Moonraker delivers.
void feed_afc_lane(AmsBackendAfc& backend, const std::string& lane_name,
                   const nlohmann::json& data) {
    nlohmann::json params;
    params["AFC_stepper " + lane_name] = data;
    nlohmann::json notification;
    notification["params"] = nlohmann::json::array({params, 0.0});
    AfcTestAccess::handle_status_update(backend, notification);
}

struct CommitFixture : LVGLTestFixture {
    MoonrakerClientMock client;
    MoonrakerAPIMock api;
    AmsBackendMock* backend = nullptr;

    CommitFixture() : api(client, get_printer_state()) {
        auto& ams = AmsState::instance();
        ams.clear_backends();
        ams.deinit_subjects();
        // AmsState::init_subjects observes PrinterState's print-state subject;
        // it must exist first or the observer attaches to nothing.
        get_printer_state().init_subjects(false);
        ams.init_subjects(false);

        // A previous test file's SpoolmanManager::deinit_subjects() may have
        // latched its shutdown flag — every static identity entry point
        // (cache_identity / find_identity / invalidate_identity) no-ops while
        // it is set. init_subjects() unlatches it.
        SpoolmanManager::instance().init_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    ~CommitFixture() override {
        auto& ams = AmsState::instance();
        ams.set_moonraker_api(nullptr);
        ams.clear_backends();
        // Drain while AmsState's subjects are still alive; queued backend-event
        // syncs from this test must not leak into the next one.
        helix::ui::UpdateQueue::instance().drain();
        ams.deinit_subjects();
        SpoolmanManager::clear_identity_cache();
    }

    /// Install a mock backend + the mock API into AmsState, seed slot 0 with
    /// spoolman_id, and return the mock API (mirrors the wiring shape of
    /// test_consumption_sink_ams.cpp / test_spoolman_identity_cache.cpp).
    MoonrakerAPIMock* setup(int spoolman_id) {
        return install(std::make_unique<AmsBackendMock>(4), spoolman_id);
    }

    /// Same, but with a backend whose manages_active_spool() reports true.
    MoonrakerAPIMock* setup_manages_active_spool(int spoolman_id) {
        auto owned = std::make_unique<ManagesActiveSpoolMock>(4);
        owned->set_afc_mode(true);
        return install(std::move(owned), spoolman_id);
    }

  private:
    MoonrakerAPIMock* install(std::unique_ptr<AmsBackendMock> owned, int spoolman_id) {
        backend = owned.get();
        auto& ams = AmsState::instance();
        ams.set_backend(std::move(owned));
        ams.set_moonraker_api(&api);

        SlotInfo slot = backend->get_slot_info(0);
        slot.spoolman_id = spoolman_id;
        backend->set_slot_info(0, slot, /*persist=*/false);
        return &api;
    }
};

} // namespace

TEST_CASE("commit_slot_edit clears server active spool on unlink", "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(169);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling.
    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the server-side active spool was cleared.
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
    // And the edit itself reached the backend slot.
    REQUIRE(f.backend->get_slot_info(0).spoolman_id == 0);
}

TEST_CASE("commit_slot_edit leaves server active spool alone on a no-link clear",
          "[ams][spoolman][commit]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(0);

    // Another lane's spool is active server-side. The unlink arm must not
    // touch it just because THIS slot's edit happened to be a clear.
    mock_api->spoolman_mock().set_active_spool(77, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 77);

    // A clear on a slot that never had a Spoolman link (original and edited
    // spoolman_id both 0): NO set_active_spool call may fire — not even a
    // clear(0), which would unlink whatever other lane the server tracks.
    // (The mock's demo data links every lane, so stage a link-less one.)
    SlotInfo seeded = f.backend->get_slot_info(1);
    seeded.material = "PLA";
    seeded.spoolman_id = 0;
    f.backend->set_slot_info(1, seeded, /*persist=*/false);

    SlotInfo original = f.backend->get_slot_info(1);
    REQUIRE(original.spoolman_id == 0);

    SlotInfo cleared = original;
    cleared.material.clear();

    AmsError err = AmsState::instance().commit_slot_edit(1, original, cleared);
    REQUIRE(err.success());

    // REQUIRED: the active spool id is UNCHANGED.
    CHECK(mock_api->spoolman_mock().get_mock_active_spool_id() == 77);
}

TEST_CASE("commit_slot_edit invalidates identity cache on link change", "[ams][spoolman][commit]") {
    CommitFixture f;
    f.setup(169);

    SpoolmanManager::cache_identity(make_spool(169, "Polymaker", "Ambrosia Pink", "PLA"));
    SpoolmanManager::cache_identity(make_spool(170, "eSUN", "Silk Blue", "PETG"));
    REQUIRE(SpoolmanManager::find_identity(169).has_value());
    REQUIRE(SpoolmanManager::find_identity(170).has_value());

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.spoolman_id = 170; // relink 169 -> 170

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the OLD spool's cached identity was dropped...
    CHECK_FALSE(SpoolmanManager::find_identity(169).has_value());
    // ...while the newly linked spool's cache entry survived untouched.
    CHECK(SpoolmanManager::find_identity(170).has_value());
}

TEST_CASE("commit_slot_edit propagates set_slot_info failure", "[ams][commit]") {
    CommitFixture f;
    f.setup(169);
    auto& ams = AmsState::instance();

    // Seed the slot subjects from the backend; the color is derived from the
    // backend, not hardcoded, so the final assertion is independent.
    const int seeded_color = static_cast<int>(f.backend->get_slot_info(0).color_rgb);
    ams.sync_from_backend();
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(0)) == seeded_color);

    // Drift the backend's slot 0 color behind AmsState's back. If a failed
    // commit still ran sync_from_backend(), this color would land in the
    // subject — that is exactly what must NOT happen.
    SlotInfo drifted = f.backend->get_slot_info(0);
    drifted.color_rgb = 0xCC2244;
    f.backend->set_slot_info(0, drifted, /*persist=*/false);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.spoolman_id = 0;

    // Slot 99 does not exist on a 4-slot backend -> set_slot_info fails.
    AmsError err = ams.commit_slot_edit(99, original, edited);

    // REQUIRED: the backend failure propagates to the caller.
    REQUIRE_FALSE(err.success());
    REQUIRE(err.result == AmsResult::INVALID_SLOT);

    // REQUIRED: sync_from_backend() was NOT re-run — the subject still shows
    // the color from the last explicit sync, not the drifted backend value.
    REQUIRE(lv_subject_get_int(ams.get_slot_color_subject(0)) == seeded_color);
}

TEST_CASE("context-menu clear wipes slot and clears server active spool",
          "[ams][commit][context-menu]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup(169);

    // Give the slot a material so the wipe itself is observable, not just the
    // unlink. The dispatch constructs its own cleared copy from get_slot_info.
    SlotInfo seeded = f.backend->get_slot_info(0);
    seeded.material = "PLA";
    f.backend->set_slot_info(0, seeded, /*persist=*/false);

    // Server thinks 169 is active — the state bundle F2LNLQCC left dangling
    // when the quick-clear only wiped the backend slot.
    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    // Drive the actual context-menu dispatch the way both AMS panels do.
    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 0, nullptr));

    // REQUIRED: the backend slot was wiped...
    const SlotInfo after = f.backend->get_slot_info(0);
    REQUIRE(after.spoolman_id == 0);
    REQUIRE(after.material.empty());
    // ...AND the server-side active spool was cleared — the F2LNLQCC fix
    // (a restart must not re-assert the cleared spool).
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
}

TEST_CASE("context-menu clear names the position in the backend's own word",
          "[ams][commit][context-menu][i18n]") {
    CommitFixture f;
    f.setup(169);

    // AmsBackendMock reports Happy Hare, whose noun is Gate. The confirmation
    // a user reads has to match the word the rest of the UI uses for the thing
    // they just cleared.
    REQUIRE(f.backend->lane_noun() == helix::ui::LaneNoun::Gate);

    std::vector<std::string> notes;
    helix::ui::set_test_notification_info_hook(
        [&notes](const std::string& msg) { notes.push_back(msg); });

    REQUIRE(
        ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL, 2, nullptr));

    helix::ui::set_test_notification_info_hook(nullptr);

    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "Gate 3 spool cleared");
}

TEST_CASE("context-menu clear on the bypass sentinel names the external spool",
          "[ams][commit][context-menu][i18n]") {
    CommitFixture f;
    f.setup(169);

    // The external spool carries an assignment, and a lane carries one too, so
    // the clear has something to remove on both sides and "it was already
    // empty" cannot pass for "it was cleared".
    SlotInfo external;
    external.material = "PETG";
    external.spoolman_id = 42;
    AmsState::instance().commit_external_spool_edit(external);
    REQUIRE(AmsState::instance().get_external_spool_info().value_or(SlotInfo{}).material == "PETG");

    SlotInfo lane = f.backend->get_slot_info(0);
    lane.material = "PLA";
    f.backend->set_slot_info(0, lane, /*persist=*/false);
    REQUIRE(f.backend->get_slot_info(0).material == "PLA");

    std::vector<std::string> notes;
    helix::ui::set_test_notification_info_hook(
        [&notes](const std::string& msg) { notes.push_back(msg); });

    REQUIRE(ui::ams_dispatch_backend_action(ui::AmsContextMenu::MenuAction::CLEAR_SPOOL,
                                            helix::ui::EXTERNAL_SPOOL_SLOT, nullptr));

    helix::ui::set_test_notification_info_hook(nullptr);

    // The bypass spool has no number, so it is named rather than numbered.
    REQUIRE(notes.size() == 1);
    CHECK(notes[0] == "External spool cleared");
    CHECK(AmsState::instance().get_external_spool_info().value_or(SlotInfo{}).material.empty());
    // The sentinel is not an index into the backend: no lane was touched.
    CHECK(f.backend->get_slot_info(0).material == "PLA");
}

TEST_CASE("commit_slot_edit clears active spool even when backend manages it",
          "[ams][spoolman][commit][regression]") {
    CommitFixture f;
    MoonrakerAPIMock* mock_api = f.setup_manages_active_spool(169);

    // Premise of the regression: the backend reports that firmware manages the
    // active spool (AFC sends SET_SPOOL_ID on load). The old
    // sync_active_spool_after_edit() (since removed) gated on this and never
    // cleared — but AFC's SET_SPOOL_ID SPOOL_ID= does NOT unlink server-side
    // either.
    REQUIRE(AmsState::instance().get_backend()->manages_active_spool());

    mock_api->spoolman_mock().set_active_spool(169, nullptr, nullptr);
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 169);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 169);

    SlotInfo edited = original;
    edited.spoolman_id = 0; // unlink

    AmsError err = AmsState::instance().commit_slot_edit(0, original, edited);
    REQUIRE(err.success());

    // REQUIRED: the clear fired anyway.
    REQUIRE(mock_api->spoolman_mock().get_mock_active_spool_id() == 0);
}

// ============================================================================
// The lane source model: what a commit records as the user's own declaration
// ============================================================================

TEST_CASE("commit_slot_edit records only the fields the user changed", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.color_rgb = 0xFFFFFF;
    original.material = "PETG";
    f.backend->set_slot_info(0, original, /*persist=*/false);
    original = f.backend->get_slot_info(0);

    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, edited).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
    // The user changed a colour, not a material. Recording the material too
    // would file a value the user never chose as their own declaration.
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("a spool link does not record the spool's colour as the user's", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 7;
    linked.brand = "Kingroon";
    linked.material = "PETG";
    linked.color_rgb = 0xFFFFFF;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 7);
    // The colour arrived with the binding. Only the server's own record may
    // assert it, so the user's record must not claim it.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->brand.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
    // The rest of what a spool carries, held to the same rule. These are the
    // fields an exception list would reach for first.
    CHECK_FALSE(sources.local_user->spoolman_vendor_id.has_value());
    CHECK_FALSE(sources.local_user->remaining_weight_g.has_value());
    CHECK_FALSE(sources.local_user->total_weight_g.has_value());
    CHECK_FALSE(sources.local_user->spool_name.has_value());
}

TEST_CASE("a field the user chose in the same commit as a link is not recorded",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo linked = original;
    linked.spoolman_id = 7;
    linked.material = "ASA"; // the person really did pick this

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, linked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 7);
    // A commit carrying a binding change cannot say which of its other fields
    // the person moved and which the binding brought, so it claims none of
    // them. The person re-picks the material as an ordinary edit, which then
    // records. This assertion is what makes that cost deliberate.
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("an unlink records the binding, not the fields it cleared", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(7);

    SlotInfo original = f.backend->get_slot_info(0);
    original.brand = "Kingroon";
    original.material = "PETG";
    original.color_rgb = 0xFFFFFF;
    f.backend->set_slot_info(0, original, /*persist=*/false);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 7);

    SlotInfo cleared = original;
    cleared.spoolman_id = 0;
    cleared.brand.clear();
    cleared.material.clear();
    cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, cleared).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The unbinding cleared those fields; the user did not choose an empty
    // material or a default colour, so neither becomes their declaration.
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->brand.has_value());
    CHECK_FALSE(sources.local_user->material.has_value());
}

TEST_CASE("an unlink after a restart stops the lane naming the unlinked spool",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(42);

    // A restart files the stored record the way every backend's start does,
    // and a record naming a spool files its whole identity as the server's.
    helix::test::file_override_as_lane_records(*f.backend, 0, linked_record(42));
    REQUIRE(helix::ams::lane_sources(lane_of(0)).spoolman.has_value());
    REQUIRE(helix::ams::resolve(helix::ams::lane_sources(lane_of(0))).spoolman_id == 42);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, unlinked).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 0);
    // The server's record describes the spool the user just unbound, and it
    // outranks the user's own record, so standing it would undo the unlink.
    const auto resolved = helix::ams::resolve(sources);
    CHECK(resolved.spoolman_id.value_or(-1) == 0);
    CHECK(resolved.brand.value_or("") != "Polymaker");
}

TEST_CASE("an edit that keeps the same spool leaves the server's record standing",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(42);
    helix::test::file_override_as_lane_records(*f.backend, 0, linked_record(42));

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    REQUIRE(original.color_rgb != 0xBCBCBC);
    SlotInfo recoloured = original;
    recoloured.color_rgb = 0xBCBCBC;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, recoloured).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
    // The binding did not move, so the server's account of the spool still
    // describes what is loaded.
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK(sources.spoolman->brand == "Polymaker");
}

TEST_CASE("an unlink the backend refuses leaves the server's record standing",
          "[ams][commit][lane][spoolman]") {
    CommitFixture f;
    f.setup(0);

    // Slot 5 is past the mock's four slots, so set_slot_info refuses it, but
    // its id is inside this backend's block, so the store holds a record there.
    helix::test::file_override_as_lane_records(*f.backend, 5, linked_record(42));
    REQUIRE(helix::ams::lane_sources(lane_of(5)).spoolman.has_value());

    SlotInfo original;
    original.spoolman_id = 42;
    SlotInfo unlinked = original;
    unlinked.spoolman_id = 0;

    REQUIRE_FALSE(AmsState::instance().commit_slot_edit(5, original, unlinked).success());

    // The edit never happened, so the spool the server describes is still the
    // one bound to the lane.
    const auto sources = helix::ams::lane_sources(lane_of(5));
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK_FALSE(sources.local_user.has_value());
}

TEST_CASE("an AFC relink after a restart survives its own echo",
          "[ams][commit][lane][spoolman][afc]") {
    CommitFixture f;
    SettingsManager::instance().init_subjects();
    helix::test::RegisteredBackend<AmsBackendAfc> afc(nullptr, nullptr);
    AfcTestAccess::initialize_slots(*afc, std::vector<std::string>{"lane1", "lane2"});
    auto& ams = AmsState::instance();
    ams.set_moonraker_api(&f.api);

    // What a restart leaves on a linked lane: the stored record in both stores,
    // and firmware naming the same spool.
    const helix::ams::FilamentSlotOverride stored = linked_record(42);
    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
        AfcTestAccess::overrides(*afc)[0] = stored;
    }
    helix::test::file_override_as_lane_records(*afc, 0, stored);
    feed_afc_lane(*afc, "lane1", {{"prep", true}, {"status", "Loaded"}, {"spool_id", 42}});
    REQUIRE(helix::ams::resolve(helix::ams::lane_sources(afc.lane(0))).spoolman_id == 42);

    SlotInfo original = afc->get_slot_info(0);
    REQUIRE(original.spoolman_id == 42);
    SlotInfo relinked = original;
    relinked.spoolman_id = 99;

    REQUIRE(ams.commit_slot_edit(0, original, relinked).success());
    CHECK(helix::ams::resolve(helix::ams::lane_sources(afc.lane(0))).spoolman_id.value_or(-1) ==
          99);

    // Firmware reports the spool we wrote. That is our own write coming back,
    // so the declaration the user just made has to survive it, in the stored
    // record and in the lane model.
    feed_afc_lane(*afc, "lane1", {{"spool_id", 99}});

    {
        std::lock_guard<std::mutex> lock(AfcTestAccess::mutex(*afc));
        const auto& overrides = AfcTestAccess::overrides(*afc);
        const auto kept = overrides.find(0);
        REQUIRE(kept != overrides.end());
        CHECK(kept->second.spoolman_id == 99);
    }

    const auto sources = helix::ams::lane_sources(afc.lane(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->spoolman_id == 99);
    CHECK(helix::ams::resolve(sources).spoolman_id == 99);
}

TEST_CASE("a later edit amends the user's record instead of replacing it", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo picked_colour = original;
    picked_colour.color_rgb = 0xBCBCBC;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, picked_colour).success());

    SlotInfo after_colour = f.backend->get_slot_info(0);
    SlotInfo picked_material = after_colour;
    picked_material.material = "ASA";
    REQUIRE(AmsState::instance().commit_slot_edit(0, after_colour, picked_material).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->material == "ASA");
    // Two statements by one person, not one statement replacing another.
    CHECK(sources.local_user->color_rgb == 0xBCBCBC);
}

TEST_CASE("a second backend's lane 0 is not the first backend's", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(ams.commit_slot_edit(0, original, edited).success());

    // Each backend owns a block of ids, which a flat slot index could not
    // express: slot 0 on one backend is not slot 0 on the other.
    CHECK(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK_FALSE(
        helix::ams::lane_sources(helix::ams::lane_id_for(second, 0)).local_user.has_value());
}

TEST_CASE("registration stamps each backend with its own block", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);

    AmsBackend* primary = ams.get_backend(0);
    AmsBackend* secondary = ams.get_backend(second);
    REQUIRE(primary != nullptr);
    REQUIRE(secondary != nullptr);

    // A backend derives its lane ids from the index registration hands it, and
    // asking a backend for its own slot 0 is the only way to see that stamp.
    // Unstamped, a backend names no lane at all, so a declaration written
    // through it would be dropped instead of filed.
    CHECK(primary->backend_index() == 0);
    CHECK(secondary->backend_index() == second);
    CHECK(primary->lane_id(0) == helix::ams::lane_id_for(0, 0));
    CHECK(secondary->lane_id(0) == helix::ams::lane_id_for(second, 0));
    CHECK(secondary->lane_id(0) != primary->lane_id(0));
    CHECK(secondary->lane_id(0) != helix::ams::INVALID_LANE_ID);
}

TEST_CASE("an edit files under the backend it was written through", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);
    auto& ams = AmsState::instance();
    const int second = ams.add_backend(std::make_unique<AmsBackendMock>(4));
    REQUIRE(second == 1);
    ams.set_active_backend(second);
    REQUIRE(ams.active_backend_index() == second);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(ams.commit_slot_edit(0, original, edited).success());

    // commit_slot_edit writes through get_backend(), which is the primary and
    // not the active one. The declaration has to name the same lane the edit
    // itself reached, or the record describes a slot nobody edited.
    CHECK(f.backend->get_slot_info(0).color_rgb == 0xBCBCBC);
    CHECK(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK_FALSE(
        helix::ams::lane_sources(helix::ams::lane_id_for(second, 0)).local_user.has_value());
}

TEST_CASE("tearing down the backends clears their lane declarations", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, edited).success());
    REQUIRE(helix::ams::lane_sources(lane_of(0)).local_user.has_value());

    // The next printer's first backend is stamped with this one's index, so a
    // declaration that outlived the backend it was made through would be read
    // back as a statement about hardware nobody edited.
    AmsState::instance().clear_backends();

    CHECK_FALSE(helix::ams::lane_sources(lane_of(0)).local_user.has_value());
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a commit the backend rejects records no declaration", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo edited = original;
    edited.color_rgb = 0xBCBCBC;

    // Slot 5 is past the mock's four slots, so set_slot_info refuses it.
    const AmsError err = AmsState::instance().commit_slot_edit(5, original, edited);
    REQUIRE_FALSE(err.success());

    // The edit reached the backend and was refused, so the user declared
    // nothing. Asked of the whole store rather than one lane: a refused commit
    // must not leave a record anywhere, including on a lane it mis-addressed.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a commit that changes nothing writes no user record", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    REQUIRE(AmsState::instance().commit_slot_edit(0, original, original).success());

    // The REQUIRE above is the proof the path ran: commit_slot_edit reached
    // the backend and the backend accepted. Having run it in full, it recorded
    // no declaration, because nothing was declared.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("clearing an already-unlinked slot declares no colour and no weight",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0); // unlinked, so the binding-change rule does not fire

    SlotInfo original = f.backend->get_slot_info(0);
    original.color_rgb = 0x1188CC;
    original.material = "PETG";
    original.remaining_weight_g = 620.0f;
    original.total_weight_g = 1000.0f;
    f.backend->set_slot_info(0, original, /*persist=*/false);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.spoolman_id == 0);

    // The shape MenuAction::CLEAR_SPOOL commits (ui_ams_detail.cpp).
    SlotInfo cleared = original;
    cleared.material.clear();
    cleared.color_rgb = AMS_DEFAULT_SLOT_COLOR;
    cleared.color_name.clear();
    cleared.brand.clear();
    cleared.clear_spoolman_link();
    cleared.remaining_weight_g = -1;
    cleared.total_weight_g = -1;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, cleared).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    // The clear is a real declaration: the material is now empty because the
    // person said so.
    REQUIRE(sources.local_user->material.has_value());
    CHECK(*sources.local_user->material == "");
    // But the sentinels are not values anyone chose. Recorded, they would
    // outrank every server reading with "no reading".
    CHECK_FALSE(sources.local_user->color_rgb.has_value());
    CHECK_FALSE(sources.local_user->remaining_weight_g.has_value());
    CHECK_FALSE(sources.local_user->total_weight_g.has_value());
}

TEST_CASE("a field cleared to empty is still the user's declaration", "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.material = "PETG";
    f.backend->set_slot_info(0, original, /*persist=*/false);
    original = f.backend->get_slot_info(0);
    REQUIRE(original.material == "PETG");

    SlotInfo emptied = original;
    emptied.material.clear();

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, emptied).success());

    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    // "Observed as empty" and "not observed" are different states, and only
    // the first of them stops a weaker source re-asserting the old material.
    REQUIRE(sources.local_user->material.has_value());
    CHECK(*sources.local_user->material == "");
}

TEST_CASE("an auto-highlighted catalog product is not the user's declaration",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    SlotInfo saved = original;
    // What an untouched open-and-Save produces: the editor preselects a
    // product and copies it in, with nothing else moved.
    saved.catalog_id = "sunlu-pla-plus-2-0";
    saved.product_name = "PLA+ 2.0";

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, saved).success());

    // Nothing was declared, so no lane was written at all.
    CHECK(helix::ams::known_lanes().empty());
}

TEST_CASE("a weight edit finer than the editor's own tolerance is not a declaration",
          "[ams][commit][lane]") {
    CommitFixture f;
    f.setup(0);

    SlotInfo original = f.backend->get_slot_info(0);
    original.remaining_weight_g = 620.0f;
    original.total_weight_g = 1000.0f;
    f.backend->set_slot_info(0, original, /*persist=*/false);
    original = f.backend->get_slot_info(0);

    SlotInfo drifted = original;
    drifted.remaining_weight_g = 619.95f; // a consumption tick, not a keystroke

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, drifted).success());
    CHECK(helix::ams::known_lanes().empty());

    SlotInfo typed = original;
    typed.remaining_weight_g = 500.0f;

    REQUIRE(AmsState::instance().commit_slot_edit(0, original, typed).success());
    const auto sources = helix::ams::lane_sources(lane_of(0));
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->remaining_weight_g == 500.0f);
}
