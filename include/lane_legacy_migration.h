// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lane_source_store.h"
#include "lane_translation.h"

namespace helix::ams {

class FilamentSlotOverrideStore;

/// File every source @p sources holds onto @p lane, each through the funnel it
/// is allowed to use: LocalUser through commit_slot_edit(), the rest through
/// ingest(). Returns true when at least one was filed.
///
/// The one list of which sources a stored record can produce. A source added
/// to LaneSources and forgotten here is filed by nobody, silently, so both the
/// real migration and the fixtures that imitate it read it from here.
bool file_lane_sources(LaneId lane, const LaneSources& sources);

/// Classify every lane_data record @p store's last load_blocking() parsed and
/// file each onto its lane.
///
/// Read-only with respect to the stored document: the legacy lock keys stay
/// on the wire and simply stop being read. sources_from_record is pure, so
/// running this on every load reaches the same lane state as running it
/// once — there is no run-once marker to get wrong and no half-migrated
/// state to recover from.
///
/// A record's LocalUser source routes through commit_slot_edit(), that
/// source's only funnel; every other source routes through ingest().
///
/// Call this once per backend, from its init path, immediately after
/// load_blocking(). request_resync() re-files only VendorCache observations
/// out of a re-read (ams_subscription_backend.cpp); calling this from a
/// resync path would re-file a LocalUser declaration out of a re-read, which
/// forges one.
///
/// @return how many lanes received at least one source.
int ingest_legacy_records(const FilamentSlotOverrideStore& store, LegacyLockKeys keys,
                          int backend_index);

} // namespace helix::ams
