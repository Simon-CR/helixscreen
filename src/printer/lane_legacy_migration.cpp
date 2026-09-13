// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_legacy_migration.h"

#include "filament_slot_override_store.h"
#include "lane_source_store.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace helix::ams {

int ingest_legacy_records(const FilamentSlotOverrideStore& store, LegacyLockKeys keys,
                          int backend_index) {
    int populated = 0;
    for (const auto& [slot_index, entry] : store.last_lane_data_records()) {
        const LaneSources sources = sources_from_record(entry.record, entry.wire, keys);
        const LaneId lane = lane_id_for(backend_index, slot_index);
        bool wrote = false;
        for (const std::optional<Observation>* held :
             {&sources.spoolman, &sources.vendor_cache, &sources.metered}) {
            if (held->has_value()) {
                ingest(lane, **held);
                wrote = true;
            }
        }
        if (sources.local_user.has_value()) {
            // ingest() refuses a LocalUser observation; commit_slot_edit() is
            // that source's only funnel.
            commit_slot_edit(lane, *sources.local_user);
            wrote = true;
        }
        if (wrote) {
            ++populated;
        }
    }
    if (populated > 0) {
        spdlog::info("[LaneMigration] Classified {} stored record(s) for backend {} into lane "
                     "sources",
                     populated, backend_index);
    }
    return populated;
}

} // namespace helix::ams
