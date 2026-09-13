// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ingest_legacy_records() is what puts a user's pre-source-model lane_data
// records into the lane source store at backend init, before anything reads
// a lane. These cases exercise it directly against a FilamentSlotOverrideStore
// loaded from a mock Moonraker DB, the same shape every backend's on_started()
// hands it.

#include "ams_types.h"
#include "filament_slot_override_store.h"
#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams::FilamentSlotOverrideStore;
using helix::ams::ingest;
using helix::ams::ingest_legacy_records;
using helix::ams::lane_id_for;
using helix::ams::lane_sources;
using helix::ams::LegacyLockKeys;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::resolved_lane;

TEST_CASE_METHOD(HelixTestFixture, "Loading a namespace populates each lane's sources",
                 "[lane][migration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{
            {"lane", 0}, {"spool_id", 7}, {"color", "#FFFFFF"}, {"helix_locked_color", true}});
    api.mock_set_db_value(
        "lane_data", "lane2",
        nlohmann::json{{"lane", 1}, {"color", "#ED2C2C"}, {"helix_material", "PLA"}});
    api.mock_set_db_value("lane_data", "seated", nlohmann::json(0));

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    const auto loaded = store.load_blocking();
    REQUIRE(loaded.size() == 2);

    const int populated =
        ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0);
    CHECK(populated == 2);

    // Lane 0 was linked, so its white landed on the server's rung despite the
    // lock flag. color_rgb is the deciding field and spoolman is the deciding
    // source.
    const auto lane0 = lane_sources(lane_id_for(0, 0));
    REQUIRE(lane0.spoolman.has_value());
    CHECK(lane0.spoolman->color_rgb == 0xFFFFFFu);
    CHECK_FALSE(lane0.local_user.has_value());

    // Lane 1 carried no lock key, so it is a cache.
    const auto lane1 = lane_sources(lane_id_for(0, 1));
    REQUIRE(lane1.remembered.has_value());
    CHECK(lane1.remembered->color_rgb == 0xED2C2Cu);
    CHECK_FALSE(lane1.local_user.has_value());

    // Nothing stored is a presence signal, so neither lane carries a presence
    // reading at all. Migration must not invent one in either direction.
    CHECK_FALSE(resolved_lane(lane_id_for(0, 0)).present.has_value());
    CHECK_FALSE(resolved_lane(lane_id_for(0, 1)).present.has_value());
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A locked, unlinked record's colour reaches the store as the user's own",
                 "[lane][migration]") {
    // The defect this task exists to prevent: ingest() silently refuses a
    // LocalUser observation, so a record with a lock key set must be routed
    // through commit_slot_edit() or the user's declaration never reaches the
    // store at all.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value(
        "lane_data", "lane1",
        nlohmann::json{{"lane", 0}, {"color", "#3355FF"}, {"helix_locked_color", true}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();

    const int populated =
        ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0);
    CHECK(populated == 1);

    const auto lane = lane_sources(lane_id_for(0, 0));
    REQUIRE(lane.local_user.has_value());
    CHECK(lane.local_user->color_rgb == 0x3355FFu);
    CHECK_FALSE(lane.remembered.has_value());
}

TEST_CASE_METHOD(HelixTestFixture, "Ingesting the same namespace twice changes nothing",
                 "[lane][migration]") {
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", 0},
                                         {"color", "#BCBCBC"},
                                         {"helix_material", "PLA"},
                                         {"helix_locked_color", true}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();
    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto first = resolved_lane(lane_id_for(0, 0));

    store.load_blocking();
    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto second = resolved_lane(lane_id_for(0, 0));

    CHECK(second.color_rgb == first.color_rgb);
    CHECK(second.material == first.material);
    CHECK(second.present == first.present);
    CHECK(second.spoolman_id == first.spoolman_id);
}

TEST_CASE_METHOD(HelixTestFixture, "Classification reads the document the store actually received",
                 "[lane][migration]") {
    // The proof that the raw side-channel is wired, not just declared: a record
    // with a colour and NO lock key must come back as a cache. Routed through
    // the parsed struct it would come back as the user's, because the parser
    // defaults the missing key to the colour's own presence.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1", nlohmann::json{{"lane", 0}, {"color", "#ED2C2C"}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    const auto loaded = store.load_blocking();

    // The parsed struct says locked. That is the trap.
    REQUIRE(loaded.at(0).user_locked_color);

    ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    const auto lane = lane_sources(lane_id_for(0, 0));
    CHECK_FALSE(lane.local_user.has_value());
    REQUIRE(lane.remembered.has_value());
    CHECK(lane.remembered->color_rgb == 0xED2C2Cu);
}

TEST_CASE_METHOD(HelixTestFixture, "A load that falls back to the on-disk cache ingests nothing",
                 "[lane][migration]") {
    // The cache-fallback path (load_blocking's offline branch) never populates
    // last_lane_data_records(): there is no wire document to classify a cached
    // record against, so ingesting it would be a guess rather than a reading.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_reject_next_db_get();

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    store.load_blocking();

    CHECK(store.last_lane_data_records().empty());
    const int populated = ingest_legacy_records(store, LegacyLockKeys::LaneData, 0);
    CHECK(populated == 0);
}

// ============================================================================
// Per-field authorship. Colour and material each carry a lock flag, so a
// record has always been able to say who chose them. Brand, spool name and
// vendor id have no flag of their own and answer from the declared set.
// ============================================================================

TEST_CASE_METHOD(HelixTestFixture,
                 "A user's typed brand outlives a firmware frame that states another",
                 "[lane][migration]") {
    // brand is editable in the spool editor (AmsEditOverlay::is_dirty), so a
    // stored brand can be the user's own word. Filing it as merely remembered
    // would put it below the machine, and the next frame carrying any brand
    // would take the lane back.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    // The edit is a brand and nothing else: every other SlotInfo field rests
    // on its "nothing here" default, so the record declares exactly one field.
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    bool saved = false;
    store.save_async(0, helix::ams::override_from_user_edit(edited),
                     [&](bool, std::string) { saved = true; });
    REQUIRE(saved);

    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(*sources.local_user->brand == "Hatchbox");

    // The machine now states a brand of its own. LocalUser outranks
    // VendorCache, so the user's word stands.
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Generic";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Hatchbox");
}

TEST_CASE_METHOD(HelixTestFixture, "A brand nobody declared yields to the next firmware frame",
                 "[lane][migration]") {
    // A record carries brands no person typed: a co-author in the shared
    // namespace writes vendor_name, and our own emit mirrors what firmware
    // said. Declaring none of them keeps the weakest rung, where a machine
    // stating the field corrects it.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    api.mock_set_db_value("lane_data", "lane1",
                          nlohmann::json{{"lane", 0},
                                         {"vendor_name", "Firmware Brand"},
                                         {"helix_locked_color", false},
                                         {"helix_locked_material", false},
                                         {"helix_declared", nlohmann::json::array()}});

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, 0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.remembered.has_value());
    REQUIRE(sources.remembered->brand.has_value());
    CHECK(*sources.remembered->brand == "Firmware Brand");

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Corrected Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Corrected Brand");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A legacy record's brand is the user's word only beside a true lock",
                 "[lane][migration]") {
    // A record with no helix_declared key was written by an older build, and
    // its brand counts as declared only when a lock flag on the same record is
    // true. That flag is the evidence a person edited the record: the
    // auto-mirror writes both flags false and can populate no brand of its own.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);

    nlohmann::json record{{"lane", 0}, {"vendor", "Hatchbox"}, {"color", "#3355FF"}};
    const bool locked = GENERATE(true, false);
    if (locked) {
        record["helix_locked_color"] = true;
    }
    api.mock_set_db_value("lane_data", "lane1", record);

    FilamentSlotOverrideStore store(&api, "ad5x_ifs");
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, 0) == 1);

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    const auto sources = lane_sources(lane);
    if (locked) {
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->brand.has_value());
        CHECK(*sources.local_user->brand == "Hatchbox");
    } else {
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->brand.has_value());
        CHECK(*sources.remembered->brand == "Hatchbox");
        CHECK_FALSE(sources.local_user.has_value());
    }

    // Either way the ranking decides the outcome, so state it at the lane.
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == (locked ? "Hatchbox" : "Firmware Brand"));
}

TEST_CASE("Colour and material answer from their lock flags in both wire formats",
          "[lane][migration]") {
    // The two fields that own a lock flag keep answering from it. lane_data is
    // a shared namespace and spells them helix_locked_*; the private cache
    // spells them bare. A record can declare one and merely remember the other.
    using helix::ams::from_json;
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;
    using helix::ams::to_json;

    SECTION("the shared lane_data record") {
        const nlohmann::json wire{{"lane", 0},
                                  {"color", "#3355FF"},
                                  {"helix_material", "PETG"},
                                  {"helix_locked_color", true},
                                  {"helix_locked_material", false}};
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());

        const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->color_rgb.has_value());
        CHECK(*sources.local_user->color_rgb == 0x3355FFu);
        CHECK_FALSE(sources.local_user->material.has_value());
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->material.has_value());
        CHECK(*sources.remembered->material == "PETG");
    }

    SECTION("the private cache record") {
        helix::ams::FilamentSlotOverride ovr;
        ovr.color_rgb = 0x3355FF;
        ovr.color_set = true;
        ovr.user_locked_color = true;
        ovr.material = "PETG";
        ovr.user_locked_material = false;

        const nlohmann::json wire = to_json(ovr);
        const auto sources = sources_from_record(from_json(wire), wire, LegacyLockKeys::LocalCache);
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->color_rgb.has_value());
        CHECK(*sources.local_user->color_rgb == 0x3355FFu);
        CHECK_FALSE(sources.local_user->material.has_value());
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->material.has_value());
        CHECK(*sources.remembered->material == "PETG");
    }
}

TEST_CASE("A declared brand survives the private cache round-trip", "[lane][migration]") {
    // The local cache is the other document a record goes home in, so it
    // carries the declared set under its own bare key.
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    const helix::ams::FilamentSlotOverride ovr = helix::ams::override_from_user_edit(edited);

    const nlohmann::json wire = helix::ams::to_json(ovr);
    REQUIRE(wire.contains("declared"));

    const auto sources = helix::ams::sources_from_record(helix::ams::from_json(wire), wire,
                                                         LegacyLockKeys::LocalCache);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(*sources.local_user->brand == "Hatchbox");
    CHECK_FALSE(sources.remembered.has_value());
}
