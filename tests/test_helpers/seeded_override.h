// tests/test_helpers/seeded_override.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seeding a stored override in a fixture, the way a backend's init would.
#pragma once

#include "ams_backend.h"
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

} // namespace helix::test
