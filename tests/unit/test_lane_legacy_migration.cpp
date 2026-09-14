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
using helix::ams::file_lane_sources;
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
    const helix::SlotInfo empty_lane;
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    bool saved = false;
    store.save_async(0, helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr),
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

TEST_CASE_METHOD(HelixTestFixture,
                 "A legacy record's true lock does not license an empty brand it never carried",
                 "[lane][migration]") {
    // The trap: "some identity field was ever locked" is not evidence THIS
    // field was ever declared. A legacy record with a locked material and no
    // brand at all must not have that lock stand in for a user's clear, or no
    // later firmware brand could ever land.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0}, {"helix_locked_material", true}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    CHECK_FALSE(sources.local_user.has_value());
    CHECK_FALSE(sources.remembered.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK_FALSE(file_lane_sources(lane, sources));

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
}

TEST_CASE_METHOD(HelixTestFixture,
                 "A field the declared set never named stays skipped when it is empty",
                 "[lane][migration]") {
    // The key being present at all must not turn every empty field into a
    // declaration - only the field the set actually names may be filed that
    // way. An empty declared set on a modern record means the same thing a
    // keyless legacy record with no lock means: the user never touched it.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;

    const nlohmann::json wire{{"lane", 0},
                              {"helix_locked_color", false},
                              {"helix_locked_material", false},
                              {"helix_declared", nlohmann::json::array()}};
    const auto parsed = from_lane_data_record(wire);
    REQUIRE(parsed.has_value());

    const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);
    CHECK_FALSE(sources.local_user.has_value());
    CHECK_FALSE(sources.remembered.has_value());

    const helix::ams::LaneId lane = lane_id_for(0, 0);
    CHECK_FALSE(file_lane_sources(lane, sources));

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    CHECK(resolved_lane(lane).brand == "Firmware Brand");
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
    const helix::SlotInfo empty_lane;
    helix::SlotInfo edited;
    edited.brand = "Hatchbox";
    const helix::ams::FilamentSlotOverride ovr =
        helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr);

    const nlohmann::json wire = helix::ams::to_json(ovr);
    REQUIRE(wire.contains("declared"));

    const auto sources = helix::ams::sources_from_record(helix::ams::from_json(wire), wire,
                                                         LegacyLockKeys::LocalCache);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(*sources.local_user->brand == "Hatchbox");
    CHECK_FALSE(sources.remembered.has_value());
}

TEST_CASE("The declared set cannot carry colour or material", "[lane][migration]") {
    // Colour and material keep their authorship on their lock flags, and the
    // auto-mirror reads those flags directly rather than through the routing
    // predicate. A second copy of either in the declared set would leave those
    // reads answering from the older of two truths, so the set holds neither
    // however a document spells itself.
    using helix::ams::from_lane_data_record;
    using helix::ams::sources_from_record;
    using helix::ams::to_json;

    SECTION("a record naming them in helix_declared does not get them declared") {
        const nlohmann::json wire{{"lane", 0},
                                  {"color", "#3355FF"},
                                  {"helix_material", "PETG"},
                                  {"vendor", "Hatchbox"},
                                  {"helix_locked_color", false},
                                  {"helix_locked_material", false},
                                  {"helix_declared", {"brand", "material", "color_rgb"}}};
        const auto parsed = from_lane_data_record(wire);
        REQUIRE(parsed.has_value());

        const auto sources = sources_from_record(parsed->second, wire, LegacyLockKeys::LaneData);

        // brand answers from the set and is the user's.
        REQUIRE(sources.local_user.has_value());
        REQUIRE(sources.local_user->brand.has_value());
        CHECK(*sources.local_user->brand == "Hatchbox");

        // Colour and material answer from their false lock flags, not from the
        // names the document put in the set.
        CHECK_FALSE(sources.local_user->material.has_value());
        CHECK_FALSE(sources.local_user->color_rgb.has_value());
        REQUIRE(sources.remembered.has_value());
        REQUIRE(sources.remembered->material.has_value());
        CHECK(*sources.remembered->material == "PETG");
        REQUIRE(sources.remembered->color_rgb.has_value());
        CHECK(*sources.remembered->color_rgb == 0x3355FFu);

        // The set never took the two names in the first place. Re-emitting it
        // is what shows that: the emitter mirrors the set without a filter of
        // its own, so a name in here would be a name the reader admitted.
        const nlohmann::json reemitted = to_json(parsed->second)["declared"];
        REQUIRE(reemitted.is_array());
        CHECK(reemitted.size() == 1);
        CHECK(reemitted.at(0) == "brand");
    }

    SECTION("an edit that supplies all three names only the one in the set") {
        const helix::SlotInfo empty_lane;
        helix::SlotInfo edited;
        edited.brand = "Hatchbox";
        edited.material = "PETG";
        edited.color_rgb = 0x3355FF;
        const auto ovr = helix::ams::user_override_from_slot_info(empty_lane, edited, nullptr);

        // The two locks carry colour and material.
        CHECK(ovr.user_locked_color);
        CHECK(ovr.user_locked_material);

        // The set carries brand and names neither of the other two.
        const nlohmann::json declared = to_json(ovr)["declared"];
        REQUIRE(declared.is_array());
        CHECK(declared.size() == 1);
        CHECK(declared.at(0) == "brand");
    }
}

// ============================================================================
// What a user's edit claims, end to end: user_override_from_slot_info builds the
// record, the store emits it, and sources_from_record routes it back.
// ============================================================================

namespace {

/// A lane carrying only what the machine reported, which is what the spool
/// editor seeds its working copy from.
helix::SlotInfo firmware_lane() {
    helix::SlotInfo info;
    info.brand = "Firmware Brand";
    info.spool_name = "Firmware Spool";
    info.spoolman_vendor_id = 3;
    info.material = "PLA";
    info.color_rgb = 0x3355FF;
    return info;
}

/// Persist @p ovr the way a backend does and put the reloaded record into the
/// lane model, exactly as a backend's on_started() would.
helix::ams::LaneId reload_into_lane(FilamentSlotOverrideStore& store,
                                    const helix::ams::FilamentSlotOverride& ovr) {
    bool saved = false;
    store.save_async(0, ovr, [&](bool, std::string) { saved = true; });
    REQUIRE(saved);
    REQUIRE(store.load_blocking().size() == 1);
    REQUIRE(ingest_legacy_records(store, LegacyLockKeys::LaneData, /*backend_index=*/0) == 1);
    return lane_id_for(0, 0);
}

/// Everything the machine states on a later frame, all of it different from
/// what firmware_lane() holds.
Observation correcting_frame() {
    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Corrected Brand";
    frame.spool_name = "Corrected Spool";
    frame.spoolman_vendor_id = 9;
    frame.material = "PETG";
    frame.color_rgb = 0xFF0000;
    return frame;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture,
                 "An edit that moves only the weight claims none of the identity it carried",
                 "[lane][migration]") {
    // The editor opens on the lane, so firmware's brand, spool name, vendor id,
    // colour and material all come back on the commit untouched. A record
    // claiming them would outrank the machine that supplied them, and no later
    // firmware correction could ever land.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.remaining_weight_g = 730.0F;

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK_FALSE(ovr.user_locked_color);
    CHECK_FALSE(ovr.user_locked_material);
    CHECK_FALSE(ovr.declared.any());
    // The identity still travels: the lane has to show it. Only the claim on it
    // does not.
    CHECK(ovr.brand == "Firmware Brand");
    CHECK(ovr.spool_name == "Firmware Spool");
    CHECK(ovr.material == "PLA");
    CHECK(ovr.color_set);

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->brand == "Firmware Brand");
    CHECK(sources.remembered->spool_name == "Firmware Spool");
    CHECK(sources.remembered->material == "PLA");

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Corrected Brand");
    CHECK(resolved.spool_name == "Corrected Spool");
    CHECK(resolved.spoolman_vendor_id == 9);
    CHECK(resolved.material == "PETG");
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture, "An edit that moves the brand claims the brand alone",
                 "[lane][migration]") {
    // One record, three fields whose authorship rides the declared set, and
    // exactly one of them moved. The record carries all three, so the routing
    // has to split them rather than answer once for the record.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.brand = "Hatchbox";

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK_FALSE(ovr.user_locked_color);
    CHECK_FALSE(ovr.user_locked_material);
    const nlohmann::json declared = helix::ams::declared_field_names(ovr.declared);
    REQUIRE(declared.is_array());
    CHECK(declared.size() == 1);
    CHECK(declared.at(0) == "brand");

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->brand == "Hatchbox");
    CHECK_FALSE(sources.local_user->spool_name.has_value());
    CHECK_FALSE(sources.local_user->spoolman_vendor_id.has_value());
    REQUIRE(sources.remembered.has_value());
    CHECK(sources.remembered->spool_name == "Firmware Spool");
    CHECK(sources.remembered->spoolman_vendor_id == 3);

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Hatchbox");
    CHECK(resolved.spool_name == "Corrected Spool");
    CHECK(resolved.spoolman_vendor_id == 9);
    CHECK(resolved.material == "PETG");
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture, "An edit that clears the brand keeps it cleared past a restart",
                 "[lane][migration]") {
    // A cleared brand is still the user's declaration, and the record's
    // declared set is the only place that survives a restart. Filing it as
    // absent would let the very next firmware frame's brand come back,
    // undoing the clear on every reload.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.brand = "";

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    const nlohmann::json declared = helix::ams::declared_field_names(ovr.declared);
    REQUIRE(declared.is_array());
    CHECK(declared.size() == 1);
    CHECK(declared.at(0) == "brand");
    CHECK(ovr.brand.empty());

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    REQUIRE(sources.local_user->brand.has_value());
    CHECK(sources.local_user->brand->empty());

    Observation frame(ObservationSource::VendorCache);
    frame.brand = "Firmware Brand";
    ingest(lane, frame);
    REQUIRE(resolved_lane(lane).brand.has_value());
    CHECK(resolved_lane(lane).brand->empty());
}

TEST_CASE_METHOD(HelixTestFixture, "An edit that moves the material locks it against the machine",
                 "[lane][migration]") {
    // The #965 protection: a material the person moved is theirs, and no
    // firmware frame may take it back.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo edited = before;
    edited.material = "ASA";

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK(ovr.user_locked_material);
    CHECK_FALSE(ovr.user_locked_color);
    CHECK_FALSE(ovr.declared.any());

    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    REQUIRE(lane_sources(lane).local_user.has_value());
    CHECK(lane_sources(lane).local_user->material == "ASA");

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.material == "ASA");
    // The colour rode along on the same commit and was never moved, so the
    // machine still owns it.
    CHECK(resolved.color_rgb == 0xFF0000u);
}

TEST_CASE_METHOD(HelixTestFixture, "A later edit keeps what an earlier edit declared",
                 "[lane][migration]") {
    // An edit speaks about the fields it moved and says nothing about the
    // rest. The colour below was chosen in the first edit and never mentioned
    // again, so a record built from the second edit alone would hand it back
    // to the machine while the lane still calls it the user's.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before = firmware_lane();
    helix::SlotInfo chose_colour = before;
    chose_colour.color_rgb = 0x1E5AA8;
    const auto first = helix::ams::user_override_from_slot_info(before, chose_colour, nullptr);
    REQUIRE(first.user_locked_color);
    REQUIRE_FALSE(first.declared.any());

    // The editor re-opens on the lane the first edit left behind, and this
    // time only the brand moves.
    helix::SlotInfo chose_brand = chose_colour;
    chose_brand.brand = "Hatchbox";
    const auto second = helix::ams::user_override_from_slot_info(chose_colour, chose_brand, &first);

    CHECK(second.user_locked_color);
    CHECK(second.color_rgb == 0x1E5AA8u);
    const nlohmann::json declared = helix::ams::declared_field_names(second.declared);
    REQUIRE(declared.is_array());
    CHECK(declared.size() == 1);
    CHECK(declared.at(0) == "brand");

    // Both come back as the user's word after a restart, not as something the
    // record merely remembered.
    const helix::ams::LaneId lane = reload_into_lane(store, second);
    const auto sources = lane_sources(lane);
    REQUIRE(sources.local_user.has_value());
    CHECK(sources.local_user->brand == "Hatchbox");
    REQUIRE(sources.local_user->color_rgb.has_value());
    CHECK(*sources.local_user->color_rgb == 0x1E5AA8u);

    ingest(lane, correcting_frame());
    const auto resolved = resolved_lane(lane);
    CHECK(resolved.brand == "Hatchbox");
    CHECK(resolved.color_rgb == 0x1E5AA8u);
    // The fields neither edit moved are still the machine's to correct.
    CHECK(resolved.material == "PETG");
    CHECK(resolved.spool_name == "Corrected Spool");
}

TEST_CASE_METHOD(HelixTestFixture, "Linking a spool declares the binding, not what rode in with it",
                 "[lane][migration]") {
    // Picking a spool fills the editor with the server's brand, name and
    // colour. The person chose a spool, not any of those values.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24);
    helix::PrinterState state;
    state.init_subjects(false);
    MoonrakerAPIMock api(client, state);
    FilamentSlotOverrideStore store(&api, "ad5x_ifs");

    const helix::SlotInfo before;
    helix::SlotInfo edited;
    edited.spoolman_id = 42;
    edited.brand = "Hatchbox";
    edited.spool_name = "Blue PETG 1kg";
    edited.spoolman_vendor_id = 7;
    edited.material = "PETG";
    edited.color_rgb = 0x3355FF;

    const auto ovr = helix::ams::user_override_from_slot_info(before, edited, nullptr);
    CHECK(ovr.spoolman_id == 42);
    CHECK_FALSE(ovr.user_locked_color);
    CHECK_FALSE(ovr.user_locked_material);
    CHECK_FALSE(ovr.declared.any());

    // A linked record is wholly the server's on reload, which is the same
    // answer by a different route, so the lane names Spoolman and no user rung.
    const helix::ams::LaneId lane = reload_into_lane(store, ovr);
    const auto sources = lane_sources(lane);
    CHECK_FALSE(sources.local_user.has_value());
    REQUIRE(sources.spoolman.has_value());
    CHECK(sources.spoolman->spoolman_id == 42);
    CHECK(sources.spoolman->brand == "Hatchbox");
}
