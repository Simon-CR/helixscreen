// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "filament_slot_override.h"
#include "lane_observation.h"
#include "lane_sources.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

namespace helix {
struct SlotInfo;
}

namespace helix::ams {

/// Which spelling of HelixScreen's authorship keys a stored document uses.
///
/// lane_data is a namespace shared with AFC, Happy Hare, Mainsail and Orca, so
/// the lock keys and the declared set carry the helix_ prefix there.
/// filament_slot_overrides.json is HelixScreen-private and uses the bare names.
/// Every reader below that checks one of those keys takes one of these so it
/// agrees with whichever document the caller actually has.
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
///
/// A field a linked spool owns is not the user's to claim either, even when
/// they moved it. On an edit that keeps a spool, material, brand, spool name
/// and Spoolman vendor id are the spool's statement, so none of them is
/// engaged; keep_spool_owned_identity() is what the commit applies in their
/// place.
///
/// spoolman_id is engaged exactly when the binding changed, and nothing else is
/// engaged alongside it. amend_authorship() reads its presence as "this edit
/// changed the binding" and voids the stored record's earlier declarations on
/// it, so engaging it on an edit that kept the spool would strip a record of
/// authorship nobody withdrew.
///
/// This answer is what keeps a lane's two stores agreeing. After an edit that
/// changes the binding (a link, a relink to a different spool, an unlink that
/// keeps the slot's identity, or Clear Spool) commits through
/// AmsState::commit_slot_edit(), the lane resolves what reloading the stored
/// record resolves, for colour, brand, material, catalog_id and product_name.
/// AmsBackend::commit_user_edit() (ams_backend.h) files the lane's half.
[[nodiscard]] Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited);

/// Who declared the identity in a stored record.
///
/// A record carrying a spool id is the server's statement. An unlinked record
/// is the user's when it declares its colour or material. The parser has
/// already read the document's authorship into the record's declared set
/// (declared_fields_on_load), so the record alone answers.
[[nodiscard]] ObservationSource classify_declaration(const FilamentSlotOverride& record);

/// The record's identity as an Observation, tagged by classify_declaration().
/// Only the fields the record actually carries are observed.
[[nodiscard]] Observation declared_from_record(const FilamentSlotOverride& record);

/// Split a stored record into the several sources it may declare independently.
///
/// declared_from_record gives a record ONE verdict, which is right the moment
/// a record is still on the wire it was just read from: a linked lane is
/// wholly the server's, and a mixed unlinked record where only one field
/// is declared is not the shape live traffic produces. A record
/// already on disk under the pre-source-model scheme does not get to make
/// that assumption: a lane can carry a declared colour beside an undeclared
/// material in the same document, and its weight is never a declaration from
/// either rung, linked or not.
///
/// Takes the already-parsed record rather than raw JSON on purpose: the two
/// document shapes this exists to migrate (lane_data, the local
/// filament_slot_overrides.json cache) disagree on almost every field's key
/// name and even the colour's wire shape (a "#RRGGBB" string vs. a bare
/// color_rgb integer), so parsing has to stay with whichever of
/// from_lane_data_record / from_json already knows the document's shape.
/// @p wire is still needed to tell a record that carries a declared set,
/// spelled per @p keys, from one written before the set existed: only the
/// latter falls back to the legacy rule for its brand, spool name and vendor
/// id. Pass the SAME document @p record was parsed from.
///
/// Pure: no clock, no globals, no I/O.
[[nodiscard]] LaneSources sources_from_record(const FilamentSlotOverride& record,
                                              const nlohmann::json& wire,
                                              LegacyLockKeys keys = LegacyLockKeys::LaneData);

/// The authorship of a record that amends @p prior with @p observed, where
/// @p amended is the record about to be stored.
///
/// One edit speaks only about the fields it moved, so what it declares is
/// added to what the lane's record already declared rather than replacing it.
/// A prior declaration the edit did not mention survives only while @p amended
/// still holds the value that declaration stood over: a value that moved
/// without this edit declaring it is no longer the user's word, and claiming
/// it would hand the machine back its own reading as something it may not
/// correct.
///
/// An edit that changes the binding, which @p observed says by carrying
/// spoolman_id, keeps none of @p prior's declarations: a different spool is on
/// the lane, so only what this edit declared stands, however many values came
/// through it unchanged. @p prior's own spool id is not consulted, because
/// firmware can bind a spool without the record being rewritten.
///
/// @p observed rather than @p amended answers what THIS edit declared, so that
/// question is decided once, by user_edit_observation(), rather than guessed
/// again from the record's values. A record holds what the lane should show,
/// which includes fields the machine supplied and the user never moved; only
/// the observation separates the two.
///
/// Every identity field comes back in the declared set, colour and material
/// included. A colour or material declaration also needs a value to stand
/// over, so a field @p amended carries nothing in is never declared however the
/// edit moved it. Brand, spool name and vendor id have no such guard, because
/// clearing one of them is itself a declaration.
///
/// A record with a spool id never declares a field the spool owns (material,
/// brand, spool name, Spoolman vendor id), whatever @p observed or @p prior
/// says: the spool's own record states it.
[[nodiscard]] RecordAuthorship amend_authorship(const Observation& observed,
                                                const FilamentSlotOverride& prior,
                                                const FilamentSlotOverride& amended);

/// @p declared as the JSON array of field names both documents persist, under
/// `helix_declared` in lane_data and `declared` in the local cache.
///
/// Names, not bit positions: the roster's order is an implementation detail
/// that a stored record must not depend on.
[[nodiscard]] nlohmann::json declared_field_names(const DeclaredFields& declared);

/// The inverse. A name with no field on this build is ignored rather than
/// refused, so a record written by a newer build still loads.
[[nodiscard]] DeclaredFields declared_fields_from_names(const nlohmann::json& names);

/// The declared set a stored record carries, for a parser that has already
/// read every other field of @p parsed off @p wire.
///
/// A field is declared when the document's set (`helix_declared`, or bare
/// `declared` in the local cache) names it, or, on a record with no spool id,
/// when @p wire carries that field's lock key present and true. The lock keys
/// are how a record written before the set could name colour and material said
/// who chose them. On a linked record they are not read, because a release 1.0
/// writer set them on links and meter flushes alike, and a missing key declares
/// nothing. Either way a colour or material declaration needs a value to stand
/// over, so neither is declared on a record holding no value for it.
[[nodiscard]] DeclaredFields declared_fields_on_load(const nlohmann::json& wire,
                                                     LegacyLockKeys keys,
                                                     const FilamentSlotOverride& parsed);

/// Whether @p record declares its colour as the user's own choice. The declared
/// set is the one home for the answer; the lock keys a stored document carries
/// are written from it.
[[nodiscard]] bool declares_color(const FilamentSlotOverride& record);

/// Whether @p record declares its material as the user's own choice.
[[nodiscard]] bool declares_material(const FilamentSlotOverride& record);

/// Withdraw @p record's colour and material declarations, leaving both values
/// where they are.
void withdraw_color_and_material(FilamentSlotOverride& record);

/// Withdraw @p record's declarations of the fields a linked spool owns
/// (material, brand, spool name, Spoolman vendor id), leaving their values
/// where they are. A record the server's own identity has just been written
/// onto claims none of them.
void withdraw_spool_owned_declarations(FilamentSlotOverride& record);

/// @p edited with every field a linked spool owns taken from the spool.
///
/// Applies to an edit that keeps the same positive spool id. Material, brand,
/// spool name and Spoolman vendor id then come from @p spool_record, the lane's
/// Spoolman record, where it names that spool and states the field, and from
/// @p original otherwise. Every other field, and every edit that changes or
/// clears the binding, passes through as @p edited has it.
[[nodiscard]] SlotInfo keep_spool_owned_identity(const SlotInfo& original, const SlotInfo& edited,
                                                 const std::optional<Observation>& spool_record);

/// The roster names, as `helix_declared` spells them, of the fields a linked
/// spool owns that @p edited moved away from @p original and @p applied does
/// not carry. Empty for an edit that changes or clears the binding.
[[nodiscard]] std::vector<std::string_view> spool_owned_fields_dropped(const SlotInfo& original,
                                                                       const SlotInfo& edited,
                                                                       const SlotInfo& applied);

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
