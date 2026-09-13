// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ingest_legacy_records() is what puts a user's pre-source-model lane_data
// records into the lane source store at backend init, before anything reads
// a lane. These cases exercise it directly against a FilamentSlotOverrideStore
// loaded from a mock Moonraker DB, the same shape every backend's on_started()
// hands it.

#include "filament_slot_override_store.h"
#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using helix::ams::FilamentSlotOverrideStore;
using helix::ams::ingest_legacy_records;
using helix::ams::lane_id_for;
using helix::ams::lane_sources;
using helix::ams::LegacyLockKeys;
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
