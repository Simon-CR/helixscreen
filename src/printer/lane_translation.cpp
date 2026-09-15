// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lane_translation.h"

#include "ams_types.h"
#include "color_utils.h"
#include "json_utils.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace helix::ams {

namespace {

/// The tolerance the spool editor itself uses to decide a weight was edited
/// (AmsEditOverlay::is_dirty). Anything finer is a consumption tick or float
/// noise, not a number a person typed into a gram field.
constexpr float WEIGHT_EPSILON_G = 0.1f;

/// True when a weight the editor committed is a number a person could have
/// entered. Negative is SlotInfo's "unknown" sentinel, and the editor refuses
/// a negative entry, so a negative can only have come from a clear.
bool is_declarable_weight(float grams) {
    return grams >= 0.0f;
}

bool weight_changed(float original, float edited) {
    return std::fabs(edited - original) > WEIGHT_EPSILON_G;
}

/// What kind of value a field holds, which is what decides whether a given
/// value counts as a declaration. The two translations ask different questions
/// of the same field - an edit has a before-value to compare against and a
/// stored record has none - so the kind names the field's shape and each
/// translation supplies its own rule for that shape.
enum class FieldKind {
    Untranslated, ///< Neither translation carries it.
    Color,        ///< Carries the AMS_DEFAULT_SLOT_COLOR "no reading" sentinel.
    Text,
    PositiveId, ///< Zero is "unset", so a stored zero declares nothing.
    Weight,     ///< Negative is the "unknown" sentinel.
};

/// Where a stored record keeps the answer to "did the user declare this?".
enum class Authorship {
    /// Nothing consults a bit for this field: either no translation carries
    /// it, or its source follows from something other than authorship.
    Unattributed,
    /// FilamentSlotOverride::declared, addressed by this row's position.
    DeclaredSet,
};

/// Whether a declaration of a field needs a value to stand over.
enum class EmptyValue {
    /// Clearing the field is itself a declaration, and the set is the one
    /// place a stored record can say the user emptied it.
    Declares,
    /// A record holding no value for the field declares nothing for it. Every
    /// mirror policy leaves a declared field alone, so a declaration over
    /// nothing would stop firmware from ever filling that lane.
    DeclaresNothing,
};

/// One field, named once for every translation that carries it. A nullptr
/// member says that translation does not claim the field, with the reason on
/// the row.
template <FieldKind K, Authorship A, EmptyValue E, typename SlotMember, typename RecordMember,
          typename ObsMember>
struct FieldRow {
    static constexpr FieldKind kind = K;
    static constexpr Authorship authorship = A;
    static constexpr EmptyValue empty_value = E;
    std::string_view name; ///< the field's name on the wire, in `helix_declared`
    SlotMember slot;       ///< SlotInfo member the edit path reads
    RecordMember record;   ///< FilamentSlotOverride member the record path reads
    ObsMember obs;         ///< where both file the value
};

template <FieldKind K, Authorship A = Authorship::Unattributed, EmptyValue E = EmptyValue::Declares,
          typename S, typename R, typename O>
constexpr auto field(std::string_view name, S slot, R record, O obs) {
    return FieldRow<K, A, E, S, R, O>{name, slot, record, obs};
}

/// Every Observation field, once. This is the field list both translations
/// walk, so a field reaches or is refused by each of them here rather than in
/// two places that agree only by convention.
constexpr auto FIELD_ROSTER = std::make_tuple(
    // Presence is sensed, never declared, so neither translation carries it.
    field<FieldKind::Untranslated>("present", nullptr, nullptr, &Observation::present),
    field<FieldKind::Color, Authorship::DeclaredSet, EmptyValue::DeclaresNothing>(
        "color_rgb", &SlotInfo::color_rgb, &FilamentSlotOverride::color_rgb,
        &Observation::color_rgb),
    // color_name has no authorship of its own: it is the colour's own text and
    // the record path files it only alongside a colour it can file.
    field<FieldKind::Text>("color_name", &SlotInfo::color_name, &FilamentSlotOverride::color_name,
                           &Observation::color_name),
    field<FieldKind::Text, Authorship::DeclaredSet, EmptyValue::DeclaresNothing>(
        "material", &SlotInfo::material, &FilamentSlotOverride::material, &Observation::material),
    field<FieldKind::Text, Authorship::DeclaredSet>(
        "brand", &SlotInfo::brand, &FilamentSlotOverride::brand, &Observation::brand),
    field<FieldKind::Text, Authorship::DeclaredSet>("spool_name", &SlotInfo::spool_name,
                                                    &FilamentSlotOverride::spool_name,
                                                    &Observation::spool_name),
    // The edit path refuses catalog_id and product_name by the rule
    // AmsEditOverlay::is_dirty() applies to them: the spool-edit view
    // auto-highlights a product and Save copies whatever is highlighted, so
    // both arrive on commits no person touched them in.
    //
    // Neither needs an authorship bit: neither firmware nor Spoolman has the
    // concept of a catalog product, so a value in either can only be a user
    // pick and the record path files it as one outright, linked or not.
    field<FieldKind::Text>("catalog_id", nullptr, &FilamentSlotOverride::catalog_id,
                           &Observation::catalog_id),
    field<FieldKind::Text>("product_name", nullptr, &FilamentSlotOverride::product_name,
                           &Observation::product_name),
    // A binding change is a whole statement the edit path answers before it
    // walks any field, so spoolman_id is not one of the fields it walks. The
    // record path answers it the same way, ahead of any per-field routing, so
    // it carries no authorship bit either.
    field<FieldKind::PositiveId>("spoolman_id", nullptr, &FilamentSlotOverride::spoolman_id,
                                 &Observation::spoolman_id),
    field<FieldKind::PositiveId, Authorship::DeclaredSet>(
        "spoolman_vendor_id", &SlotInfo::spoolman_vendor_id,
        &FilamentSlotOverride::spoolman_vendor_id, &Observation::spoolman_vendor_id),
    // A weight is a measurement wherever it came from, so the record path
    // files both as Metered without asking who wrote them.
    field<FieldKind::Weight>("remaining_weight_g", &SlotInfo::remaining_weight_g,
                             &FilamentSlotOverride::remaining_weight_g,
                             &Observation::remaining_weight_g),
    field<FieldKind::Weight>("total_weight_g", &SlotInfo::total_weight_g,
                             &FilamentSlotOverride::total_weight_g, &Observation::total_weight_g));

static_assert(std::tuple_size_v<decltype(FIELD_ROSTER)> ==
                  std::tuple_size_v<decltype(std::declval<Observation&>().fields())>,
              "every Observation field needs a row above: a field with no row is dropped by "
              "both translations while the store amends it, with nothing red");

static_assert(std::tuple_size_v<decltype(FIELD_ROSTER)> <= DeclaredFields::CAPACITY,
              "DeclaredFields addresses a roster row per bit, so the roster may not outgrow it");

template <typename Fn> void for_each_field(Fn&& fn) {
    std::apply([&fn](const auto&... rows) { (fn(rows), ...); }, FIELD_ROSTER);
}

/// As above, also handing each row its own position, which is the bit
/// DeclaredFields keeps that row's authorship in. A fold over the comma
/// operator evaluates left to right, so the counter tracks the roster order.
template <typename Fn> void for_each_field_indexed(Fn&& fn) {
    std::apply(
        [&fn](const auto&... rows) {
            size_t index = 0;
            ((fn(rows, index), ++index), ...);
        },
        FIELD_ROSTER);
}

/// The roster position of the row named @p name, for the two fields whose
/// authorship the record path has to reach by name rather than by walking.
template <size_t I = 0> constexpr size_t index_of(std::string_view name) {
    if constexpr (I < std::tuple_size_v<decltype(FIELD_ROSTER)>) {
        return std::get<I>(FIELD_ROSTER).name == name ? I : index_of<I + 1>(name);
    } else {
        return std::tuple_size_v<decltype(FIELD_ROSTER)>;
    }
}

constexpr size_t COLOR_INDEX = index_of("color_rgb");
constexpr size_t MATERIAL_INDEX = index_of("material");
static_assert(COLOR_INDEX < std::tuple_size_v<decltype(FIELD_ROSTER)>, "colour row went missing");
static_assert(MATERIAL_INDEX < std::tuple_size_v<decltype(FIELD_ROSTER)>,
              "material row went missing");

/// Every roster position whose authorship the declared set carries. The two
/// walks that build a set admit these rows and no others, so this is also the
/// full set of bits any DeclaredFields can hold.
constexpr uint16_t declared_set_mask() {
    uint16_t mask = 0;
    size_t index = 0;
    std::apply(
        [&](const auto&... rows) {
            ((mask |= (std::decay_t<decltype(rows)>::authorship == Authorship::DeclaredSet
                           ? static_cast<uint16_t>(uint16_t{1} << index)
                           : uint16_t{0}),
              ++index),
             ...);
        },
        FIELD_ROSTER);
    return mask;
}

// Colour and material keep their authorship in the declared set beside every
// other identity field. declares_color, declares_material and the lock keys
// both emitters write all read these two bits, so a row that left the set
// would leave every one of them answering from a bit nothing sets.
static_assert((declared_set_mask() & (uint16_t{1} << COLOR_INDEX)) != 0,
              "colour's authorship lives in the declared set: its roster row must be "
              "Authorship::DeclaredSet");
static_assert((declared_set_mask() & (uint16_t{1} << MATERIAL_INDEX)) != 0,
              "material's authorship lives in the declared set: its roster row must be "
              "Authorship::DeclaredSet");

/// True when this row's translation does not carry the field.
template <typename Member> constexpr bool skipped = std::is_null_pointer_v<Member>;

/// True when @p record holds a value a declaration of row @p f can stand over.
/// A row whose empty value is itself a declaration always does.
template <typename Row>
bool can_declare([[maybe_unused]] const Row& f,
                 [[maybe_unused]] const FilamentSlotOverride& record) {
    if constexpr (Row::empty_value == EmptyValue::Declares) {
        return true;
    } else if constexpr (Row::kind == FieldKind::Color) {
        // color_set is the record's own "a colour is present" flag, and
        // color_rgb is undefined while it is false.
        return record.color_set && is_declarable_color(record.*(f.record));
    } else {
        return !(record.*(f.record)).empty();
    }
}

struct LockKeyNames {
    const char* color;
    const char* material;
};

constexpr LockKeyNames lock_key_names(LegacyLockKeys keys) {
    return keys == LegacyLockKeys::LocalCache
               ? LockKeyNames{"user_locked_color", "user_locked_material"}
               : LockKeyNames{"helix_locked_color", "helix_locked_material"};
}

/// True only when @p key is present on @p wire and says true. An absent, null
/// or false key is not a declaration of authorship.
bool locked(const nlohmann::json& wire, const char* key) {
    return wire.contains(key) && helix::json_util::safe_bool(wire, key, false);
}

/// Same split as lock_key_names, for the declared set: lane_data is shared, so
/// the key carries the helix_ prefix there and the bare name in our own cache.
constexpr const char* declared_key_name(LegacyLockKeys keys) {
    return keys == LegacyLockKeys::LocalCache ? "declared" : "helix_declared";
}

/// File a stored record's catalog pick on @p user, answering whether the
/// record held one. Neither firmware nor Spoolman has the concept of a catalog
/// product, so a value in either field is a person's pick whatever the record
/// declares or the binding says, and the editor reopens on the exact product from it.
bool file_catalog_pick(const FilamentSlotOverride& record, Observation& user) {
    if (!record.catalog_id.empty()) {
        user.catalog_id = record.catalog_id;
    }
    if (!record.product_name.empty()) {
        user.product_name = record.product_name;
    }
    return !record.catalog_id.empty() || !record.product_name.empty();
}

/// The merge rule, for one field, and the only place it is spelled out.
///
/// What this edit declares is the user's word outright. What the record
/// already declared stays theirs only while the value that declaration stood
/// over is still the one the record holds: a value that moved with no
/// declaration behind the move belongs to whoever moved it.
constexpr bool amended_declaration(bool declares_now, bool declared_before, bool value_unchanged) {
    return declares_now || (declared_before && value_unchanged);
}

} // namespace

Observation user_edit_observation(const SlotInfo& original, const SlotInfo& edited) {
    Observation obs(ObservationSource::LocalUser);

    // A binding change is a statement about the binding, not about the fields
    // that rode in with it. Linking a spool carries the spool's colour, brand
    // and material into the same commit, and unlinking clears them; in neither
    // direction did a person choose those values, so neither direction may
    // file them as the person's own declaration.
    if (edited.spoolman_id != original.spoolman_id) {
        obs.spoolman_id = edited.spoolman_id;
        return obs;
    }

    // The user's statement is what moved. A field that reads the same as the
    // editor opened on is not theirs to claim, whatever it holds.
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.slot)>) {
            const auto& before = original.*(f.slot);
            const auto& after = edited.*(f.slot);
            if constexpr (Row::kind == FieldKind::Weight) {
                if (is_declarable_weight(after) && weight_changed(before, after))
                    obs.*(f.obs) = after;
            } else if constexpr (Row::kind == FieldKind::Color) {
                if (after != before && is_declarable_color(after))
                    obs.*(f.obs) = after;
            } else {
                // A cleared text field and a zeroed id are both moves a person
                // made, so an empty value here is a declaration, not an absence.
                if (after != before)
                    obs.*(f.obs) = after;
            }
        }
    });
    return obs;
}

ObservationSource classify_declaration(const FilamentSlotOverride& record) {
    if (record.spoolman_id > 0) {
        return ObservationSource::Spoolman;
    }
    // Remembered, not VendorCache: every caller of this hands it a record read
    // back from our own store, never a frame the machine just sent. VendorCache
    // is what firmware states now, and a backend replaces that record whole on
    // each parse, so a stored record filed there loses every field the next
    // frame is silent about.
    return (declares_color(record) || declares_material(record)) ? ObservationSource::LocalUser
                                                                 : ObservationSource::Remembered;
}

Observation declared_from_record(const FilamentSlotOverride& record) {
    Observation obs(classify_declaration(record));

    // A stored record has no before-value, so what it carries is what it
    // declares. Every field defaults to something value-shaped (empty string,
    // 0, -1.0f), and only a value past that default is a statement.
    for_each_field([&](const auto& f) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (!skipped<decltype(f.record)>) {
            const auto& value = record.*(f.record);
            if constexpr (Row::kind == FieldKind::Color) {
                // color_set is the record's own "a colour is present" flag and
                // has no Observation field to occupy a row, so the one colour
                // row reads it directly. color_rgb is undefined while it is false.
                if (record.color_set && is_declarable_color(value))
                    obs.*(f.obs) = value;
            } else if constexpr (Row::kind == FieldKind::Text) {
                if (!value.empty())
                    obs.*(f.obs) = value;
            } else if constexpr (Row::kind == FieldKind::PositiveId) {
                if (value > 0)
                    obs.*(f.obs) = value;
            } else {
                if (is_declarable_weight(value))
                    obs.*(f.obs) = value;
            }
        }
    });
    return obs;
}

LaneSources sources_from_record(const FilamentSlotOverride& record, const nlohmann::json& wire,
                                LegacyLockKeys keys) {
    LaneSources sources;

    // A weight is a measurement, never a declaration, whether the lane is
    // linked or not: the meter and Spoolman both refresh it independently of
    // who owns the rest of the record's identity.
    if (is_declarable_weight(record.remaining_weight_g) ||
        is_declarable_weight(record.total_weight_g)) {
        Observation metered(ObservationSource::Metered);
        if (is_declarable_weight(record.remaining_weight_g)) {
            metered.remaining_weight_g = record.remaining_weight_g;
        }
        if (is_declarable_weight(record.total_weight_g)) {
            metered.total_weight_g = record.total_weight_g;
        }
        sources.apply(metered);
    }

    if (record.spoolman_id > 0) {
        // A linked lane's identity is the server's, so what its record
        // declares is not consulted here. declared_from_record walks every
        // field, so two kinds are stripped back off the server's record: the
        // weights, already filed above, and the catalog pick, which a spool
        // record cannot state. A fetch replaces the server's record whole, so
        // the pick is filed as the user's or it would not survive the first.
        Observation server = declared_from_record(record);
        server.remaining_weight_g.reset();
        server.total_weight_g.reset();
        server.catalog_id.reset();
        server.product_name.reset();
        sources.apply(server);

        Observation user(ObservationSource::LocalUser);
        if (file_catalog_pick(record, user)) {
            sources.apply(user);
        }
        return sources;
    }

    // Colour and material each carry their own bit, so one record can declare
    // one field and merely remember the other.
    const bool color_declared = declares_color(record);

    // A record written by an older build carries no declared key, and its
    // brand, spool name and vendor id count as declared only beside a colour or
    // material declaration on the same record. That declaration is the
    // evidence a person edited the record at all: the auto-mirror declares
    // neither field and can populate none of those three, so a record holding a
    // brand beside one got it from an edit. Reading every value a legacy record
    // happens to hold as a declaration would instead pin a mirrored firmware
    // brand as the user's word, and no later firmware correction could ever
    // land on it.
    const bool has_declared = wire.contains(declared_key_name(keys));
    const bool legacy_declared = color_declared || declares_material(record);

    // Whether the user declared the field at roster position `index`. Colour
    // and material answer from their bits whatever the record's age, because
    // the parser already read an older record's lock keys into them; the rest
    // fall back to the legacy rule on a record with no set.
    const auto declared_field = [&](size_t index) {
        if (has_declared || index == COLOR_INDEX || index == MATERIAL_INDEX) {
            return record.declared.test(index);
        }
        return legacy_declared;
    };

    // Everything this record merely REMEMBERS, as opposed to declares, is
    // filed as Remembered rather than VendorCache. VendorCache is what the
    // machine states on the current frame, and a backend replaces that record
    // whole on every parse, so a field this record holds and the frame does
    // not restate would be erased on the first poll after load.
    Observation user(ObservationSource::LocalUser);
    Observation remembered(ObservationSource::Remembered);
    bool have_user = false;
    bool have_remembered = false;

    if (record.color_set && is_declarable_color(record.color_rgb)) {
        Observation& target = color_declared ? user : remembered;
        target.color_rgb = record.color_rgb;
        if (!record.color_name.empty()) {
            target.color_name = record.color_name;
        }
        (color_declared ? have_user : have_remembered) = true;
    }
    // Every remaining field whose source turns on who wrote it. The colour is
    // not among them: its row is walked above, together with the colour name
    // that only travels when there is a colour to travel with.
    for_each_field_indexed([&](const auto& f, size_t index) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (Row::authorship != Authorship::Unattributed &&
                      Row::kind != FieldKind::Color) {
            const auto& value = record.*(f.record);
            bool carried = false;
            if constexpr (Row::kind == FieldKind::Text) {
                carried = !value.empty();
            } else {
                carried = value > 0;
            }
            if (!carried) {
                // An empty value is still a declaration when the record's own
                // declared set names this field and clearing the field is a
                // declaration at all: that is the one place a stored record
                // can tell a field the user cleared apart from a field that was
                // simply never set. legacy_declared answers a different
                // question - was colour or material ever declared - and cannot
                // make that distinction, so only the set itself, never its
                // legacy stand-in, may file an empty value here.
                if constexpr (Row::empty_value == EmptyValue::Declares) {
                    if (has_declared && record.declared.test(index)) {
                        user.*(f.obs) = value;
                        have_user = true;
                    }
                }
                return;
            }
            const bool is_declared = declared_field(index);
            Observation& target = is_declared ? user : remembered;
            target.*(f.obs) = value;
            (is_declared ? have_user : have_remembered) = true;
        }
    });

    if (file_catalog_pick(record, user)) {
        have_user = true;
    }

    if (have_user) {
        sources.apply(user);
    }
    if (have_remembered) {
        sources.apply(remembered);
    }
    return sources;
}

RecordAuthorship amend_authorship(const Observation& observed, const FilamentSlotOverride& prior,
                                  const FilamentSlotOverride& amended) {
    RecordAuthorship authorship;

    // A changed binding says another spool is on the lane, so nothing the
    // record declared describes what is loaded now, whichever values came
    // through unchanged: this edit's own statement is all that stands.
    // user_edit_observation() engages spoolman_id exactly when the binding
    // moved. The prior record's id cannot answer the same question, because
    // firmware can bind a spool without the record being rewritten.
    const FilamentSlotOverride nothing_declared;
    const FilamentSlotOverride& standing =
        observed.spoolman_id.has_value() ? nothing_declared : prior;

    // One rule for every roster row that keeps its authorship in the declared
    // set. No per-kind rule for what this edit declares: the observation was
    // built by comparing the edit against what it opened on, so a field it
    // carries is one a person moved, whatever value they moved it to. Clearing
    // a brand is a declaration the same as typing into one, and the set is the
    // one home that can say so.
    //
    // A colour or material declaration also needs a value to stand over, so
    // the amended record has to carry one for either half of the rule to set
    // the bit. An observation only ever carries a declarable colour, but a
    // material arrives empty from a clear and from a backend whose normalized
    // spelling came back empty.
    for_each_field_indexed([&](const auto& f, size_t index) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (Row::authorship == Authorship::DeclaredSet) {
            if (!can_declare(f, amended)) {
                return;
            }
            bool value_unchanged = standing.*(f.record) == amended.*(f.record);
            if constexpr (Row::kind == FieldKind::Color) {
                // color_rgb means nothing while color_set is false, so the two
                // are one value.
                value_unchanged = value_unchanged && standing.color_set == amended.color_set;
            }
            if (amended_declaration((observed.*(f.obs)).has_value(), standing.declared.test(index),
                                    value_unchanged)) {
                authorship.declared.set(index);
            }
        }
    });
    return authorship;
}

nlohmann::json declared_field_names(const DeclaredFields& declared) {
    // A faithful mirror of the set, with no filter of its own. Refusing a field
    // here as well would mean two places decide what the set may hold, and
    // either one could then stop working without anything to show for it. The
    // roster walks that build a set are where that is decided.
    nlohmann::json names = nlohmann::json::array();
    for_each_field_indexed([&](const auto& f, size_t index) {
        if (declared.test(index)) {
            names.push_back(std::string(f.name));
        }
    });
    return names;
}

DeclaredFields declared_fields_from_names(const nlohmann::json& names) {
    DeclaredFields declared;
    if (!names.is_array()) {
        return declared;
    }
    for (const auto& entry : names) {
        if (!entry.is_string()) {
            continue;
        }
        const std::string name = entry.get<std::string>();
        // Only rows that keep their authorship here are admitted, so a name
        // the roster files some other way, such as a catalog pick, sets
        // nothing.
        //
        // A name this build has no row for is a field a newer one declares.
        // Dropping it loses only authorship this build could not act on
        // anyway, where refusing the whole record would lose the rest of it.
        for_each_field_indexed([&](const auto& f, size_t index) {
            using Row = std::decay_t<decltype(f)>;
            if constexpr (Row::authorship == Authorship::DeclaredSet) {
                if (f.name == name) {
                    declared.set(index);
                }
            }
        });
    }
    return declared;
}

DeclaredFields declared_fields_on_load(const nlohmann::json& wire, LegacyLockKeys keys,
                                       const FilamentSlotOverride& parsed) {
    const char* const set_key = declared_key_name(keys);
    DeclaredFields declared;
    if (wire.contains(set_key)) {
        declared = declared_fields_from_names(wire[set_key]);
    }

    // A lock key speaks only for an unlinked record: a release 1.0 writer set
    // both keys on every link and meter flush, so on a linked record they say
    // nothing a person chose.
    if (parsed.spoolman_id <= 0) {
        const LockKeyNames lock = lock_key_names(keys);
        if (locked(wire, lock.color)) {
            declared.set(COLOR_INDEX);
        }
        if (locked(wire, lock.material)) {
            declared.set(MATERIAL_INDEX);
        }
    }

    // Whichever spelling named a colour or material, the declaration stands
    // only over a value the record holds.
    for_each_field_indexed([&](const auto& f, size_t index) {
        using Row = std::decay_t<decltype(f)>;
        if constexpr (Row::authorship == Authorship::DeclaredSet) {
            if (!can_declare(f, parsed)) {
                declared.reset(index);
            }
        }
    });
    return declared;
}

bool declares_color(const FilamentSlotOverride& record) {
    return record.declared.test(COLOR_INDEX);
}

bool declares_material(const FilamentSlotOverride& record) {
    return record.declared.test(MATERIAL_INDEX);
}

void withdraw_color_and_material(FilamentSlotOverride& record) {
    record.declared.reset(COLOR_INDEX);
    record.declared.reset(MATERIAL_INDEX);
}

bool is_declarable_color(uint32_t rgb) {
    return rgb != AMS_DEFAULT_SLOT_COLOR;
}

ColorReading read_lane_color(const std::string& raw) {
    // A value that is only whitespace and a prefix carries no colour to fail
    // to parse, so it is the producer saying the lane has none.
    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    if (begin < end && raw[begin] == '#') {
        ++begin;
    }
    if (begin == end) {
        return {ColorReadingKind::Cleared, 0};
    }

    if (const auto rgb = parse_hex_color(raw)) {
        return {ColorReadingKind::Observed, *rgb};
    }
    return {ColorReadingKind::NoReading, 0};
}

} // namespace helix::ams
