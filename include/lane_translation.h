// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_slot_override.h"
#include "lane_observation.h"
#include "lane_sources.h"

#include <cstdint>
#include <string>

#include "hv/json.hpp"

namespace helix {
struct SlotInfo;
}

namespace helix::ams {

/// Which spelling of the lock-flag keys a stored document uses.
///
/// lane_data is a namespace shared with AFC, Happy Hare, Mainsail and Orca, so
/// our lock flags carry the helix_ prefix there. filament_slot_overrides.json
/// is HelixScreen-private and uses the bare names. Every reader below that
/// checks a lock key takes one of these so it agrees with whichever document
/// the caller actually has.
enum class LegacyLockKeys {
    LaneData,   ///< "helix_locked_color" / "helix_locked_material"
    LocalCache, ///< "user_locked_color" / "user_locked_material"
};

/// The user's statement in one edit: the fields that differ between what the
/// editor opened on and what it committed, minus the differences no person
/// caused.
///
/// A field the user did not move is not theirs to claim, and a plain diff of
/// two snapshots claims three kinds of field they did not move:
///   - a binding change carries the spool's own values, so a commit that
///     changes spoolman_id states the binding and nothing else, in both
///     directions;
///   - a cleared field lands on a sentinel (AMS_DEFAULT_SLOT_COLOR, a
///     negative weight) that means "no reading", never a chosen value;
///   - catalog_id and product_name arrive from the editor's auto-highlighted
///     product, which is why AmsEditOverlay::is_dirty() excludes them too.
[[nodiscard]] Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited);

/// Who declared the identity in a stored record.
///
/// A record carrying a spool id is the server's statement and its lock flags
/// are not read: on a linked lane those flags record that a colour rode in on
/// the binding, not that a person chose it. An unlinked record is the user's
/// only when a lock key is actually present in @p wire, spelled per @p keys;
/// the parsed struct defaults a missing key from color_set, so the struct
/// alone cannot tell a declaration from a legacy colour.
[[nodiscard]] ObservationSource
classify_declaration(const FilamentSlotOverride& record, const nlohmann::json& wire,
                     LegacyLockKeys keys = LegacyLockKeys::LaneData);

/// The record's identity as an Observation, tagged by classify_declaration().
/// Only the fields the record actually carries are observed.
[[nodiscard]] Observation declared_from_record(const FilamentSlotOverride& record,
                                               const nlohmann::json& wire,
                                               LegacyLockKeys keys = LegacyLockKeys::LaneData);

/// Split a stored record into the several sources it may declare independently.
///
/// declared_from_record gives a record ONE verdict, which is right the moment
/// a record is still on the wire it was just read from: a linked lane is
/// wholly the server's, and a mixed unlinked record where only one field
/// carries a lock key is not the shape live traffic produces. A record
/// already on disk under the pre-source-model scheme does not get to make
/// that assumption: a lane can carry a locked colour beside an unlocked
/// material in the same document, and its weight is never a declaration from
/// either rung, linked or not.
///
/// Takes the already-parsed record rather than raw JSON on purpose: the two
/// document shapes this exists to migrate (lane_data, the local
/// filament_slot_overrides.json cache) disagree on almost every field's key
/// name and even the colour's wire shape (a "#RRGGBB" string vs. a bare
/// color_rgb integer), so parsing has to stay with whichever of
/// from_lane_data_record / from_json already knows the document's shape.
/// @p wire is still needed for the lock-key presence check, same reason
/// declared_from_record needs it: pass the SAME document @p record was
/// parsed from, or a lock key that happens to be absent reads as a
/// declaration-free cache when the source document actually set it.
///
/// Pure: no clock, no globals, no I/O.
[[nodiscard]] LaneSources sources_from_record(const FilamentSlotOverride& record,
                                              const nlohmann::json& wire,
                                              LegacyLockKeys keys = LegacyLockKeys::LaneData);

/// The declared set a user's edit records: every field @p observed carries
/// whose authorship has no lock flag of its own.
///
/// Takes the observation rather than the record it is about to become, so that
/// what a person declared is decided once, by user_edit_observation(), and read
/// here rather than guessed again from the record's values. A record holds what
/// the lane should show, which includes fields the machine supplied and the
/// user never moved; only the observation separates the two.
///
/// Colour and material are deliberately absent. FilamentSlotOverride's
/// user_locked_color / user_locked_material are their declared bits, and those
/// flags are load-bearing past authorship - a reader of the shared lane_data
/// namespace keys on their presence to recognise a record as HelixScreen's.
/// sources_from_record reads each field from whichever of the two homes it
/// uses, so no caller has to know which is which.
[[nodiscard]] DeclaredFields declared_fields_supplied(const Observation& observed);

/// @p declared as the JSON array of field names both documents persist, under
/// `helix_declared` in lane_data and `declared` in the local cache.
///
/// Names, not bit positions: the roster's order is an implementation detail
/// that a stored record must not depend on.
[[nodiscard]] nlohmann::json declared_field_names(const DeclaredFields& declared);

/// The inverse. A name with no field on this build is ignored rather than
/// refused, so a record written by a newer build still loads.
[[nodiscard]] DeclaredFields declared_fields_from_names(const nlohmann::json& names);

/// What a lane-shaped record's colour string says.
enum class ColorReadingKind {
    Observed,  ///< A colour, in ColorReading::rgb.
    Cleared,   ///< The producer states this lane has no colour.
    NoReading, ///< The producer said something that is not a colour.
};

/// Three answers, not two, because the consumers need different ones: SlotInfo
/// has no uint32_t that means "leave this alone", and an Observation's unset
/// field means "not observed", which is not the same statement as "this lane
/// has no colour".
struct ColorReading {
    ColorReadingKind kind;
    uint32_t rgb{0}; ///< Meaningful only when kind is Observed.
};

/// The colour @p raw states, for a caller that has already pulled the string
/// off whatever key its own wire spells it under.
///
/// It takes the string rather than the record because the key is the wire's
/// business and differs per producer, where the rule for reading the value
/// does not. The hex grammar is helix::parse_hex_color's, so `#RGB`, bare
/// `RRGGBB`, an `0x` prefix and the 8-digit `#RRGGBBAA` slicers emit all mean
/// here what they mean everywhere else in the tree, and a value with anything
/// else in it is refused rather than half-read.
///
/// Empty, or nothing but a `#`, is Cleared: a producer wiping a lane writes
/// the key empty, and AFC's SET_COLOR with no value stores the bare prefix.
///
/// This answers "what did the producer say", which is not the question
/// is_declarable_color (declared below) answers. AMS_DEFAULT_SLOT_COLOR
/// written on a wire is a producer stating a grey and reads as Observed here;
/// the same value sitting in a struct is that struct's "no reading" sentinel
/// and is refused there.
[[nodiscard]] ColorReading read_lane_color(const std::string& raw);

/// True when a colour resting in a SlotInfo-shaped struct is a reading rather
/// than that struct's own "no colour" default. AMS_DEFAULT_SLOT_COLOR is where
/// a cleared slot and a colourless record both land, so filing it would hand
/// every one of them a declared grey.
///
/// This is the struct-side counterpart of read_lane_color, and deliberately a
/// different answer: a producer writing #808080 on a WIRE is stating a grey,
/// where a struct resting on its default is not. A translation that already
/// holds the producer's string asks read_lane_color; one whose only access to
/// the colour is a decoded uint32_t asks this.
[[nodiscard]] bool is_declarable_color(uint32_t rgb);

} // namespace helix::ams
