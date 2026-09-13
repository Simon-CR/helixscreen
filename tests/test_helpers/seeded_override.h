// tests/test_helpers/seeded_override.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeding a stored override in a fixture, the way a backend's init would.
#pragma once

#include "ams_backend.h"
#include "ams_types.h"
#include "filament_slot_override.h"
#include "lane_legacy_migration.h"
#include "lane_source_store.h"
#include "lane_translation.h"

#include "hv/json.hpp"

namespace helix::test {

/// File the lane records a stored override would have produced at load.
///
/// Every backend's on_started() pairs load_blocking() with
/// ingest_legacy_records(), so the application never holds an override whose
/// lane records are missing. A fixture that writes overrides_ alone models a
/// state the application cannot be in, and anything reading through the lane
/// model then sees an empty lane and leaves the backend's own values standing.
///
/// Routes each source through the funnel that source is allowed to use, and
/// classifies with the same pure sources_from_record() the real migration
/// uses, so a fixture cannot disagree with production about what a record
/// means. There is no wire JSON in a seeded record, so the legacy lock keys
/// are absent and the classification rests on the record's own fields.
///
/// @p backend must be registered with AmsState, or lane_id() answers
/// INVALID_LANE_ID and the funnels drop every record. RegisteredBackend is how
/// a fixture gets that.
inline void file_override_as_lane_records(const AmsBackend& backend, int slot_index,
                                          const helix::ams::FilamentSlotOverride& ovr) {
    const helix::ams::LaneSources sources = helix::ams::sources_from_record(
        ovr, nlohmann::json::object(), helix::ams::LegacyLockKeys::LaneData);
    helix::ams::file_lane_sources(backend.lane_id(slot_index), sources);
}

/// Edit a slot the way the application does.
///
/// AmsState::commit_slot_edit writes the backend and THEN records the user's
/// statement in the lane model, using the same before/after pair the editor
/// had. A fixture that calls set_slot_info alone performs only the first half,
/// so the lane keeps whatever it held and the edit has nothing standing behind
/// it.
///
/// Deliberately reuses user_edit_observation rather than filing every field:
/// that function decides what a person actually claimed, and a fixture that
/// claimed more than production does would pass on a stronger declaration than
/// the application ever files.
inline void edit_slot_as_user(AmsBackend& backend, int slot_index, const helix::SlotInfo& info) {
    const helix::SlotInfo original = backend.get_slot_info(slot_index);
    backend.set_slot_info(slot_index, info, /*persist=*/true);
    helix::ams::commit_slot_edit(backend.lane_id(slot_index),
                                 helix::ams::user_edit_observation(original, info));
}

/// The identity a linked spool carries, as Spoolman states it.
///
/// Linking is a statement about the binding, so user_edit_observation files the
/// id alone; the brand, colour and material that rode in with it are the
/// server's word, and in the application SpoolmanManager files them. A fixture
/// that links a spool without this has a lane naming an id nothing describes.
inline void spool_states(const AmsBackend& backend, int slot_index, const helix::SlotInfo& info) {
    helix::ams::Observation server(helix::ams::ObservationSource::Spoolman);
    if (info.spoolman_id > 0) {
        server.spoolman_id = info.spoolman_id;
    }
    if (!info.brand.empty()) {
        server.brand = info.brand;
    }
    if (!info.spool_name.empty()) {
        server.spool_name = info.spool_name;
    }
    if (!info.material.empty()) {
        server.material = info.material;
    }
    if (helix::ams::is_declarable_color(info.color_rgb)) {
        server.color_rgb = info.color_rgb;
    }
    // Spoolman owns a linked spool's consumption, so the weights are its word
    // too and Moonraker decrements them there.
    if (info.remaining_weight_g >= 0.0F) {
        server.remaining_weight_g = info.remaining_weight_g;
    }
    if (info.total_weight_g >= 0.0F) {
        server.total_weight_g = info.total_weight_g;
    }
    helix::ams::ingest(backend.lane_id(slot_index), server);
}

} // namespace helix::test
