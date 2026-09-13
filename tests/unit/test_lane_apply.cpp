// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "helix_test_fixture.h"
#include "lane_apply.h"
#include "lane_observation.h"
#include "lane_source_store.h"

#include "../catch_amalgamated.hpp"

using helix::SlotInfo;
using helix::SlotStatus;
using helix::ams::apply_resolved;
using helix::ams::narrow_status;
using helix::ams::Observation;
using helix::ams::ObservationSource;
using helix::ams::resolved_lane;
using helix::ams::ResolvedLane;

TEST_CASE("Resolved presence is the only thing that decides whether a lane is occupied",
          "[lane][apply]") {
    const SlotStatus every[] = {SlotStatus::UNKNOWN, SlotStatus::EMPTY,   SlotStatus::AVAILABLE,
                                SlotStatus::LOADED,  SlotStatus::BLOCKED, SlotStatus::FROM_BUFFER};

    SECTION("no backend status can make an absent lane read occupied") {
        for (SlotStatus s : every) {
            INFO("backend status " << helix::slot_status_to_string(s));
            CHECK(narrow_status(s, false) == SlotStatus::EMPTY);
        }
    }

    SECTION("no backend status can make a present lane read absent") {
        for (SlotStatus s : every) {
            INFO("backend status " << helix::slot_status_to_string(s));
            const SlotStatus out = narrow_status(s, true);
            CHECK(out != SlotStatus::EMPTY);
            CHECK(out != SlotStatus::UNKNOWN);
        }
    }

    SECTION("a present lane keeps the flavour its backend state machine owns") {
        // LOADED, BLOCKED and FROM_BUFFER are not presence answers: they say
        // which lane is at the extruder, that a lane is jammed, and Happy Hare's
        // gate_status 2 respectively. The resolver has no opinion on any of them.
        CHECK(narrow_status(SlotStatus::LOADED, true) == SlotStatus::LOADED);
        CHECK(narrow_status(SlotStatus::BLOCKED, true) == SlotStatus::BLOCKED);
        CHECK(narrow_status(SlotStatus::FROM_BUFFER, true) == SlotStatus::FROM_BUFFER);
        CHECK(narrow_status(SlotStatus::AVAILABLE, true) == SlotStatus::AVAILABLE);
    }

    SECTION("a present lane whose backend has no flavour yet reads available") {
        CHECK(narrow_status(SlotStatus::UNKNOWN, true) == SlotStatus::AVAILABLE);
        CHECK(narrow_status(SlotStatus::EMPTY, true) == SlotStatus::AVAILABLE);
    }
}

TEST_CASE("apply_resolved writes the resolver's fields and leaves the rest alone",
          "[lane][apply]") {
    SlotInfo slot;
    slot.slot_index = 3;
    slot.global_index = 7;
    slot.status = SlotStatus::LOADED;
    slot.mapped_tool = 2;
    slot.extruder_name = "extruder1";
    slot.endless_spool_group = 4;
    slot.remaining_length_m = 12.5F;
    slot.nozzle_temp_min = 210;
    slot.material = "PETG";
    slot.color_rgb = 0xFFFFFF;
    // Fields apply_resolved must leave alone because ResolvedLane has no
    // equivalent of them: a later task must not quietly start writing these.
    slot.multi_color_hexes = "#111111,#222222";
    slot.spoolman_filament_id = 99;

    ResolvedLane r;
    r.present = true;
    r.color_rgb = 0xBCBCBC;
    r.color_name = "Gunmetal";
    r.material = "PLA";
    r.brand = "Kingroon";
    r.spool_name = "Reel 9";
    r.catalog_id = "cat-kingroon-pla";
    r.product_name = "PLA Silk";
    r.spoolman_id = 7;
    r.spoolman_vendor_id = 12;
    r.remaining_weight_g = 218.0F;
    r.total_weight_g = 950.0F;

    apply_resolved(slot, r);

    // The resolver's 11 fields are replaced wholesale, each with a value
    // distinctive enough that a dropped assignment cannot pass by accident.
    CHECK(slot.color_rgb == 0xBCBCBC);
    CHECK(slot.color_name == "Gunmetal");
    CHECK(slot.material == "PLA");
    CHECK(slot.brand == "Kingroon");
    CHECK(slot.spool_name == "Reel 9");
    CHECK(slot.catalog_id == "cat-kingroon-pla");
    CHECK(slot.product_name == "PLA Silk");
    CHECK(slot.spoolman_id == 7);
    CHECK(slot.spoolman_vendor_id == 12);
    CHECK(slot.remaining_weight_g == Catch::Approx(218.0F));
    CHECK(slot.total_weight_g == Catch::Approx(950.0F));

    // Presence narrows the status; LOADED is a flavour, so it survives.
    CHECK(slot.status == SlotStatus::LOADED);

    // Everything SlotInfo carries that the resolver does not own is untouched,
    // including the two fields ResolvedLane has no member for at all.
    CHECK(slot.slot_index == 3);
    CHECK(slot.global_index == 7);
    CHECK(slot.mapped_tool == 2);
    CHECK(slot.extruder_name == "extruder1");
    CHECK(slot.endless_spool_group == 4);
    CHECK(slot.remaining_length_m == Catch::Approx(12.5F));
    CHECK(slot.nozzle_temp_min == 210);
    CHECK(slot.multi_color_hexes == "#111111,#222222");
    CHECK(slot.spoolman_filament_id == 99);
}

TEST_CASE("A lane the resolver reports absent is emptied, not merely dimmed", "[lane][apply]") {
    SlotInfo slot;
    slot.status = SlotStatus::LOADED;
    slot.material = "PETG";

    ResolvedLane r;
    r.present = false;
    r.material = "PETG"; // identity survives an eject by design (#1071)

    apply_resolved(slot, r);

    CHECK(slot.status == SlotStatus::EMPTY);
    CHECK_FALSE(slot.is_present());
    // Identity is deliberately retained so the lane ghosts rather than blanking.
    CHECK(slot.material == "PETG");
}

TEST_CASE_METHOD(HelixTestFixture, "resolved_lane reflects what was ingested for that lane",
                 "[lane][apply]") {
    Observation sensed(ObservationSource::Sensed);
    sensed.present = true;
    helix::ams::ingest(0, sensed);

    Observation cache(ObservationSource::VendorCache);
    cache.color_rgb = 0xED2C2C;
    cache.material = "PETG";
    helix::ams::ingest(0, cache);

    const ResolvedLane r = resolved_lane(0);
    CHECK(r.present == true);
    CHECK(r.color_rgb == 0xED2C2C);
    CHECK(r.material == "PETG");
}

TEST_CASE_METHOD(HelixTestFixture, "an unwritten lane resolves to an empty, default reading",
                 "[lane][apply]") {
    const ResolvedLane r = resolved_lane(5);
    CHECK(r.present == false);
    CHECK(r.material.empty());
    CHECK(r.color_rgb == helix::AMS_DEFAULT_SLOT_COLOR);
}
