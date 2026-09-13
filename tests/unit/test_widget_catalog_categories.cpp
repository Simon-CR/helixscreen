// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_widget_catalog_categories.cpp
 * @brief Add Widget catalog: category grouping and two-level teardown (#1016)
 *
 * The catalog is a flat file-static state machine with three teardown paths that
 * must each fire on_close exactly once, and it now has a sub-page pushed on top
 * of it. These tests pin both halves: that the grouping partitions the registry
 * exactly, and that diving in and out never loses or double-fires the callback
 * GridEditMode relies on to un-hide its dots overlay.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_update_queue.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "config.h"
#include "display_settings_manager.h"
#include "grid_layout.h"
#include "panel_widget_config.h"
#include "panel_widget_registry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Direct children of a container — the catalog builds one row per entry, so the
/// child count IS the row count.
uint32_t child_count(lv_obj_t* obj) {
    return obj ? lv_obj_get_child_count(obj) : 0;
}

/// header_bar renders its title with text_transform="uppercase", so the rendered
/// string is compared case-insensitively against the category name.
std::string upper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Independent mirror of the catalog's gate rule: a def is unavailable when its
/// gate subject exists and reads 0. Derived here from the registry and the live
/// subjects so the tests never trust the implementation's own helper.
bool def_is_gated_here(const PanelWidgetDef& def) {
    if (!def.hardware_gate_subject) {
        return false;
    }
    lv_subject_t* gate = lv_xml_get_subject(nullptr, def.hardware_gate_subject);
    return gate && lv_subject_get_int(gate) == 0;
}

/// Open every hardware gate the fixture has subjects for.
void open_all_gates() {
    for (const auto& def : get_all_widget_defs()) {
        if (lv_subject_t* gate = def.hardware_gate_subject
                                     ? lv_xml_get_subject(nullptr, def.hardware_gate_subject)
                                     : nullptr) {
            lv_subject_set_int(gate, 1);
        }
    }
}

/// The label text of a setting_action_row.
std::string row_label(lv_obj_t* row) {
    lv_obj_t* label = lv_obj_find_by_name(row, "label");
    return label ? lv_label_get_text(label) : std::string();
}

/// Find a row inside @p group whose label is exactly @p text (labels render
/// lv_tr'd English under the fixture's reset language).
lv_obj_t* row_with_label(lv_obj_t* group, const std::string& text) {
    if (!group) {
        return nullptr;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(group); i++) {
        lv_obj_t* row = lv_obj_get_child(group, static_cast<int32_t>(i));
        if (row && row_label(row) == text) {
            return row;
        }
    }
    return nullptr;
}

/// Defs of @p category readable as available here, in registry order — derived
/// without the overlay's own helpers.
std::vector<std::string> available_ids_in_category(WidgetCategory category) {
    std::vector<std::string> ids;
    for (const auto& def : get_all_widget_defs()) {
        if (def.category == category && !def_is_gated_here(def)) {
            ids.push_back(def.id);
        }
    }
    return ids;
}

/// Every label text anywhere under a widget.
void collect_labels(lv_obj_t* obj, std::vector<std::string>& out) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
        if (!child) {
            continue;
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            out.push_back(lv_label_get_text(child) ? lv_label_get_text(child) : "");
        }
        collect_labels(child, out);
    }
}

} // namespace

// ============================================================================
// Fixture
// ============================================================================

class WidgetCatalogCategoryFixture : public LVGLUITestFixture {
  public:
    WidgetCatalogCategoryFixture() {
        // Overlay close callbacks reach us either synchronously (animations off)
        // or via lv_async_call after the slide-out completes. Switching
        // animations off takes the wall-clock timing out of the assertions
        // without changing which callback runs — both paths converge on the same
        // registered close callback. Restored in the destructor: it is a
        // process-wide setting and the next test did not ask for it.
        animations_were_enabled_ = DisplaySettingsManager::instance().get_animations_enabled();
        DisplaySettingsManager::instance().set_animations_enabled(false);

        // Production keeps the active root panel at panel_stack_[0], and overlay
        // code reads panel_stack_.back() to find what sits beneath. Without this
        // the second push still looks like the first.
        for (auto& p : root_panels_) {
            p = lv_obj_create(lv_screen_active());
        }
        NavigationManager::instance().set_panels(root_panels_.data());
        NavigationManager::instance().set_active(PanelId::Home);
        settle();

        ConfigTestAccess::data(config_) = json::object();
        widget_config_ = std::make_unique<PanelWidgetConfig>("home", config_);
        widget_config_->load();
    }

    ~WidgetCatalogCategoryFixture() override {
        // Never leave the file-static catalog state open — the next test's
        // show() would warn and no-op.
        force_close();
        // The teardown chain is queue -> queue -> lv_obj_delete_async, so a
        // single drain leaves a deletion armed against widgets this fixture is
        // about to free. settle() runs both halves to exhaustion.
        settle();
        // NavigationManager holds panel_widgets_ / panel_stack_ pointers into the
        // screen the base fixture is about to delete.
        NavigationManager::instance().deinit_subjects();
        DisplaySettingsManager::instance().set_animations_enabled(animations_were_enabled_);
    }

    /// Run the queue until the two-level teardown has fully unwound.
    ///
    /// go_back() runs its whole body through UpdateQueue, and the catalog's own
    /// pop is queued *behind* the sub-page's pop, so the second pop is only
    /// enqueued once the first batch has run. One drain is never enough.
    void settle() {
        for (int i = 0; i < 4; i++) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(10);
        }
    }

    void force_close() {
        auto& nav = NavigationManager::instance();
        for (int i = 0; i < 4 && WidgetCatalogOverlay::active_root(); i++) {
            nav.go_back();
            settle();
        }
    }

    /// Open the catalog and let the push land.
    void open_catalog() {
        WidgetCatalogOverlay::show(
            lv_screen_active(), *widget_config_,
            [this](const std::string& id) { selected_ids_.push_back(id); },
            [this]() { close_count_++; });
        settle();
    }

    /// The <setting_group> holding the top-level category rows.
    static lv_obj_t* category_group() {
        lv_obj_t* root = WidgetCatalogOverlay::active_root();
        return root ? lv_obj_find_by_name(root, "category_group") : nullptr;
    }

    /// The sub-page scroll container holding one row per widget in the category.
    static lv_obj_t* category_scroll() {
        lv_obj_t* page = WidgetCatalogOverlay::active_category_root();
        return page ? lv_obj_find_by_name(page, "catalog_scroll") : nullptr;
    }

    /// Tap the top-level row whose label is a category's display name.
    void dive_category(const WidgetCategoryDef& cat) {
        lv_obj_t* group = category_group();
        REQUIRE(group != nullptr);
        lv_obj_t* row = row_with_label(group, cat.display_name);
        INFO("category row: " << cat.display_name);
        REQUIRE(row != nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        settle();
    }

    /// Tap the trailing "Unavailable on this printer" row.
    void dive_unavailable() {
        lv_obj_t* group = category_group();
        REQUIRE(group != nullptr);
        lv_obj_t* row = row_with_label(group, "Unavailable on this printer");
        REQUIRE(row != nullptr);
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        settle();
    }

    /// Type into the search box — lv_textarea_set_text fires the same
    /// value_changed the XML event_cb listens for.
    void type_query(const std::string& text) {
        lv_obj_t* ta =
            lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "catalog_search_input");
        REQUIRE(ta != nullptr);
        lv_textarea_set_text(ta, text.c_str());
        settle();
    }

    lv_obj_t* browse_list() {
        return lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "catalog_scroll");
    }

    /// The search level container — the widget the view subject actually hides
    /// and shows. search_results is a child of it and never flips its own flag.
    lv_obj_t* search_level() {
        return lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "search_level");
    }

    lv_obj_t* search_results() {
        return lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "search_results");
    }

    lv_obj_t* search_empty_message() {
        return lv_obj_find_by_name(WidgetCatalogOverlay::active_root(), "search_empty");
    }

    /// Children of @p parent not carrying HIDDEN.
    static uint32_t visible_rows(lv_obj_t* parent) {
        uint32_t visible = 0;
        for (uint32_t i = 0; parent && i < lv_obj_get_child_count(parent); i++) {
            lv_obj_t* row = lv_obj_get_child(parent, static_cast<int32_t>(i));
            if (row && !lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
                visible++;
            }
        }
        return visible;
    }

    /// Back out of whatever is on top — what the inherited header back button does.
    void header_back() {
        NavigationManager::instance().go_back();
        settle();
    }

    Config config_;
    std::unique_ptr<PanelWidgetConfig> widget_config_;
    std::array<lv_obj_t*, UI_PANEL_COUNT> root_panels_{};
    int close_count_ = 0;
    bool animations_were_enabled_ = true;
    std::vector<std::string> selected_ids_;
};

// ============================================================================
// Grouping — pure, no overlay needed
// ============================================================================

TEST_CASE("Widget catalog: every def belongs to exactly one category", "[widget_catalog][1016]") {
    const auto& defs = get_all_widget_defs();
    const auto& categories = get_widget_categories();
    REQUIRE_FALSE(defs.empty());
    REQUIRE_FALSE(categories.empty());

    std::multiset<std::string> seen;
    for (const auto& cat : categories) {
        for (const auto* def : WidgetCatalogOverlay::widgets_in_category(cat.id)) {
            seen.insert(def->id);
        }
    }

    // Nothing orphaned, nothing duplicated: the categories partition the registry.
    CHECK(seen.size() == defs.size());
    for (const auto& def : defs) {
        INFO("widget id: " << def.id);
        CHECK(seen.count(def.id) == 1);
    }
}

TEST_CASE("Widget catalog: a category holds exactly its own defs, in registry order",
          "[widget_catalog][1016]") {
    const auto& defs = get_all_widget_defs();

    for (const auto& cat : get_widget_categories()) {
        // Derived independently of widgets_in_category() — a filter that returned
        // everything, or reordered, fails here.
        std::vector<std::string> expected;
        for (const auto& def : defs) {
            if (def.category == cat.id) {
                expected.push_back(def.id);
            }
        }

        std::vector<std::string> actual;
        for (const auto* def : WidgetCatalogOverlay::widgets_in_category(cat.id)) {
            actual.push_back(def->id);
        }

        INFO("category: " << cat.display_name);
        CHECK(actual == expected);
        CHECK_FALSE(actual.empty()); // an empty category would be a dead menu row
    }
}

TEST_CASE("Widget catalog: every category def resolves and is uniquely identified",
          "[widget_catalog][1016]") {
    std::set<int> ids;
    for (const auto& cat : get_widget_categories()) {
        const WidgetCategoryDef* found = find_widget_category(cat.id);
        REQUIRE(found != nullptr);
        CHECK(found->id == cat.id);
        CHECK(std::string(found->display_name) == cat.display_name);
        REQUIRE(found->icon != nullptr);
        CHECK(std::string(found->icon).size() > 0);
        CHECK(ids.insert(static_cast<int>(cat.id)).second);
    }
}

// ============================================================================
// Top level
// ============================================================================

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: top level lists categories plus an unavailable row, not widgets",
                 "[widget_catalog][1016]") {
    open_catalog();
    REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);

    lv_obj_t* group = category_group();
    REQUIRE(group != nullptr);

    // Derived independently: one row per category holding at least one
    // available def, plus the unavailable row while any gate is closed here.
    size_t expect_categories = 0;
    for (const auto& cat : get_widget_categories()) {
        if (!available_ids_in_category(cat.id).empty()) {
            expect_categories++;
        }
    }
    size_t gated = 0;
    for (const auto& def : get_all_widget_defs()) {
        gated += def_is_gated_here(def);
    }
    CHECK(child_count(group) == expect_categories + (gated > 0 ? 1 : 0));
    // The whole point of #1016: the flat 37-row list is gone.
    CHECK(child_count(group) < get_all_widget_defs().size());
    if (gated > 0) {
        CAPTURE(gated);
        REQUIRE(row_with_label(group, "Unavailable on this printer") != nullptr);
    }
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: each category row dives into that category's available widgets",
                 "[widget_catalog][1016]") {
    const auto& categories = get_widget_categories();

    for (const auto& cat : categories) {
        const auto expected_ids = available_ids_in_category(cat.id);
        if (expected_ids.empty()) {
            continue; // no row is rendered for a fully-unavailable category
        }
        open_catalog();
        dive_category(cat);

        lv_obj_t* scroll = category_scroll();
        INFO("category: " << cat.display_name);
        REQUIRE(scroll != nullptr);
        CHECK(child_count(scroll) == expected_ids.size());

        // Rows are named for their def ids, so the page's exact membership is
        // checkable member by member — gated defs must not leak in.
        for (const auto& id : expected_ids) {
            INFO("widget id: " << id);
            CHECK(lv_obj_find_by_name(scroll, id.c_str()) != nullptr);
        }
        size_t gated_here = 0;
        for (const auto& def : get_all_widget_defs()) {
            if (def.category == cat.id && def_is_gated_here(def)) {
                gated_here++;
                CHECK(lv_obj_find_by_name(scroll, def.id) == nullptr);
            }
        }
        CHECK(gated_here + expected_ids.size() ==
              WidgetCatalogOverlay::widgets_in_category(cat.id).size());

        // The sub-page wears the category's own name. overlay_panel bakes the
        // title at parse time, so this only holds if the props reach it.
        lv_obj_t* page = WidgetCatalogOverlay::active_category_root();
        REQUIRE(page != nullptr);
        lv_obj_t* title = lv_obj_find_by_name(page, "header_title");
        REQUIRE(title != nullptr);
        CHECK(upper(lv_label_get_text(title)) == upper(cat.display_name));

        force_close();
        CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    }
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: category rows count available widgets, not registry members",
                 "[widget_catalog][1016]") {
    open_catalog();
    lv_obj_t* group = category_group();
    REQUIRE(group != nullptr);

    // The subtitle must advertise what the dive actually offers: the available
    // count, not the registry membership (gated defs file under the unavailable
    // row instead).
    for (const auto& cat : get_widget_categories()) {
        const size_t available = available_ids_in_category(cat.id).size();
        if (available == 0) {
            continue;
        }
        lv_obj_t* row = row_with_label(group, cat.display_name);
        INFO("category: " << cat.display_name);
        REQUIRE(row != nullptr);
        lv_obj_t* desc = lv_obj_find_by_name(row, "description");
        REQUIRE(desc != nullptr);
        CHECK(lv_label_get_text(desc) == std::to_string(available) + " widgets");
    }
    const size_t gated = [&] {
        size_t n = 0;
        for (const auto& def : get_all_widget_defs()) {
            n += def_is_gated_here(def);
        }
        return n;
    }();
    if (gated > 0) {
        lv_obj_t* row = row_with_label(group, "Unavailable on this printer");
        REQUIRE(row != nullptr);
        lv_obj_t* desc = lv_obj_find_by_name(row, "description");
        REQUIRE(desc != nullptr);
        CHECK(lv_label_get_text(desc) == std::to_string(gated) + " widgets");
    }

    force_close();
}

// ============================================================================
// Teardown
// ============================================================================

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: backing out of a category keeps the catalog open",
                 "[widget_catalog][1016]") {
    open_catalog();
    lv_obj_t* root = WidgetCatalogOverlay::active_root();
    REQUIRE(root != nullptr);

    const auto& categories = get_widget_categories();
    REQUIRE(categories.size() >= 2);
    dive_category(categories[0]);
    REQUIRE(WidgetCatalogOverlay::active_category_root() != nullptr);
    CHECK(close_count_ == 0);

    header_back();

    // Back at the category list: the sub-page is gone, the catalog survives it,
    // and GridEditMode has NOT been told the catalog closed.
    CHECK(WidgetCatalogOverlay::active_category_root() == nullptr);
    CHECK(WidgetCatalogOverlay::active_root() == root);
    CHECK(close_count_ == 0);
    CHECK(NavigationManager::instance().is_panel_on_top(root));

    // And the category list is still usable — a second dive works.
    dive_category(categories[1]);
    CHECK(WidgetCatalogOverlay::active_category_root() != nullptr);
    CHECK(close_count_ == 0);
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: closing from the top level fires on_close exactly once",
                 "[widget_catalog][1016]") {
    open_catalog();
    REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);

    header_back();

    CHECK(close_count_ == 1);
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);

    // Extra unwinding must not fire it again.
    header_back();
    CHECK(close_count_ == 1);
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: closing from a dived-in state fires on_close exactly once",
                 "[widget_catalog][1016]") {
    open_catalog();
    dive_category(get_widget_categories()[0]);
    REQUIRE(WidgetCatalogOverlay::active_category_root() != nullptr);

    // Two levels, two backs: out of the category, then out of the catalog.
    header_back();
    CHECK(close_count_ == 0);

    header_back();
    CHECK(close_count_ == 1);
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    CHECK(WidgetCatalogOverlay::active_category_root() == nullptr);
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: selecting a widget inside a category closes both levels",
                 "[widget_catalog][1016]") {
    // Find a single-instance, ungated widget and make sure it reads as unplaced,
    // so its row is clickable rather than dimmed.
    const PanelWidgetDef* target = nullptr;
    const WidgetCategoryDef* target_cat = nullptr;
    for (const auto& cat : get_widget_categories()) {
        for (const auto* def : WidgetCatalogOverlay::widgets_in_category(cat.id)) {
            if (!def->multi_instance && def->hardware_gate_subject == nullptr) {
                target = def;
                target_cat = &cat;
                break;
            }
        }
        if (target) {
            break;
        }
    }
    REQUIRE(target != nullptr);
    REQUIRE(target_cat != nullptr);

    // Force the target off so the catalog treats it as placeable.
    auto& entries = widget_config_->mutable_entries();
    for (size_t i = 0; i < entries.size(); i++) {
        if (entries[i].id == std::string(target->id)) {
            widget_config_->set_enabled(i, false);
        }
    }
    REQUIRE_FALSE(widget_config_->is_enabled(target->id));

    open_catalog();
    dive_category(*target_cat);

    lv_obj_t* scroll = category_scroll();
    REQUIRE(scroll != nullptr);
    // Rows are named for their def ids.
    lv_obj_t* row = lv_obj_find_by_name(scroll, target->id);
    REQUIRE(row != nullptr);

    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    settle();

    // The selection is reported exactly once, with the id of the row tapped.
    REQUIRE(selected_ids_.size() == 1);
    CHECK(selected_ids_[0] == std::string(target->id));

    // And both overlays are gone, with the close reported exactly once.
    CHECK(close_count_ == 1);
    CHECK(WidgetCatalogOverlay::active_root() == nullptr);
    CHECK(WidgetCatalogOverlay::active_category_root() == nullptr);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
}

// ============================================================================
// Hardware gating applies to multi-instance widgets too
// ============================================================================

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: a gated multi-instance widget is dimmed and unselectable",
                 "[widget_catalog][1016]") {
    // power_device and thermistor are the only defs that are BOTH multi_instance
    // and hardware-gated. The multi-instance branch used to hardcode the gate
    // off, so on a printer with no Moonraker power device the Power row rendered
    // bright and clickable and minted an instance that could never work.
    // The target must also have a live gate subject in this fixture — some gate
    // subjects (e.g. temp_sensor_count) are not registered here, and a missing
    // subject reads as available, which is not the branch under test.
    const PanelWidgetDef* target = nullptr;
    const WidgetCategoryDef* target_cat = nullptr;
    for (const auto& def : get_all_widget_defs()) {
        if (def.multi_instance && def.hardware_gate_subject &&
            lv_xml_get_subject(nullptr, def.hardware_gate_subject) != nullptr) {
            target = &def;
            for (const auto& cat : get_widget_categories()) {
                if (cat.id == def.category) {
                    target_cat = &cat;
                }
            }
            break;
        }
    }
    REQUIRE(target != nullptr);
    REQUIRE(target_cat != nullptr);
    INFO("gated multi-instance widget: " << target->id);

    // Close the gate: the subject must exist, or the catalog reads "available"
    // and this test would pass for the wrong reason.
    lv_subject_t* gate = lv_xml_get_subject(nullptr, target->hardware_gate_subject);
    REQUIRE(gate != nullptr);
    lv_subject_set_int(gate, 0);

    // Its own category must not offer it any more — that is the whole point of
    // the unavailable section: categories list only placeable widgets.
    open_catalog();
    REQUIRE(row_with_label(category_group(), "Unavailable on this printer") != nullptr);
    dive_category(*target_cat);
    lv_obj_t* cat_scroll = category_scroll();
    REQUIRE(cat_scroll != nullptr);
    CHECK(lv_obj_find_by_name(cat_scroll, target->id) == nullptr);
    header_back();

    // The unavailable section carries it, dimmed with its reason.
    dive_unavailable();
    lv_obj_t* scroll = category_scroll();
    REQUIRE(scroll != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(scroll, target->id);
    REQUIRE(row != nullptr);

    // The row name carries the gate hint, and the row is dimmed and not tappable.
    std::vector<std::string> labels;
    collect_labels(row, labels);
    CHECK(std::any_of(labels.begin(), labels.end(),
                      [](const std::string& l) { return l.find(" (") != std::string::npos; }));
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(row, LV_PART_MAIN) < LV_OPA_COVER);

    // Tapping it must not mint an instance.
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(selected_ids_.empty());

    force_close();
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: no unavailable row while every gate is open",
                 "[widget_catalog][1016]") {
    open_all_gates();
    open_catalog();
    REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);

    lv_obj_t* group = category_group();
    REQUIRE(group != nullptr);
    // One row per category, nothing else — the unavailable row only exists to
    // carry gated widgets, and there are none.
    CHECK(child_count(group) == get_widget_categories().size());
    CHECK(row_with_label(group, "Unavailable on this printer") == nullptr);
    force_close();
}

// ============================================================================
// Size badge units
// ============================================================================

/// The deepest label in a row's right-hand group is the size badge. "Placed"
/// sits in the same group as a bare label, so the badge is found by its own
/// container rather than by position among siblings.
static std::string find_badge_text(lv_obj_t* row) {
    std::string best;
    std::function<void(lv_obj_t*)> walk = [&](lv_obj_t* obj) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
            lv_obj_t* child = lv_obj_get_child(obj, static_cast<int32_t>(i));
            if (lv_obj_check_type(child, &lv_label_class)) {
                const char* txt = lv_label_get_text(child);
                if (txt && std::string(txt).find('x') != std::string::npos) {
                    best = txt;
                }
            }
            walk(child);
        }
    };
    walk(row);
    return best;
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: size badges are cells, not half-cell tracks",
                 "[widget_catalog][1016]") {
    // The registry stores spans in tracks and a track is half a cell. Printing
    // them raw badged every one-cell widget as "2x2" and disagreed with both the
    // grid the user sees and the sizes the user guide documents.
    const auto& cats = get_widget_categories();
    open_catalog();

    auto check_page_rows = [&](const std::vector<const PanelWidgetDef*>& defs) {
        for (const auto* def : defs) {
            lv_obj_t* scroll = category_scroll();
            REQUIRE(scroll != nullptr);
            // Rows are named for their def ids; gated rows carry the hint in the
            // name text but keep the id as their widget name.
            lv_obj_t* row = lv_obj_find_by_name(scroll, def->id);
            INFO("widget " << def->id);
            REQUIRE(row != nullptr);

            const std::string badge = find_badge_text(row);
            INFO("badge '" << badge << "'");
            REQUIRE_FALSE(badge.empty());

            const int col_cells = def->colspan / GridLayout::TRACKS_PER_CELL;
            const int row_cells = def->rowspan / GridLayout::TRACKS_PER_CELL;
            std::string expect = std::to_string(col_cells) +
                                 (def->colspan % GridLayout::TRACKS_PER_CELL ? ".5" : "") + "x" +
                                 std::to_string(row_cells) +
                                 (def->rowspan % GridLayout::TRACKS_PER_CELL ? ".5" : "");
            CHECK(badge == expect);

            // The mutation this guards against: raw track counts. Every shipping
            // widget is at least one whole cell, so a raw span always differs.
            CHECK(badge != std::to_string(def->colspan) + "x" + std::to_string(def->rowspan));
        }
    };

    bool checked_any = false;
    for (const auto& cat : cats) {
        dive_category(cat);
        const auto defs = WidgetCatalogOverlay::widgets_in_category(cat.id);
        // The category page shows only its available defs; the gated ones are
        // checked through the unavailable page below so every def gets exactly
        // one badge check.
        std::vector<const PanelWidgetDef*> available;
        for (const auto* def : defs) {
            if (!def_is_gated_here(*def)) {
                available.push_back(def);
            }
        }
        check_page_rows(available);
        checked_any = checked_any || !available.empty();
        header_back();
    }
    // Any def the fixture gates lands here instead — same badge rule.
    std::vector<const PanelWidgetDef*> gated;
    for (const auto& def : get_all_widget_defs()) {
        if (def_is_gated_here(def)) {
            gated.push_back(&def);
        }
    }
    if (!gated.empty()) {
        dive_unavailable();
        check_page_rows(gated);
        checked_any = true;
        header_back();
    }

    CHECK(checked_any);
    force_close();
}

// ============================================================================
// Search (flat type-to-filter over the whole registry)
// ============================================================================

namespace {

/// Defs whose translated-name group or description contains @p query, derived
/// straight from the registry — the same English the fixture's lv_tr() returns,
/// so the expectation never routes through the implementation.
std::set<std::string> expected_search_hits(const std::string& query) {
    const std::string q = lower(query);
    std::set<std::string> hits;
    for (const auto& def : get_all_widget_defs()) {
        const WidgetCategoryDef* cat = find_widget_category(def.category);
        const std::string label = def.display_name ? def.display_name : def.id;
        const std::string group = cat ? cat->display_name : "";
        const std::string desc = def.description ? def.description : "";
        if (lower(label).find(q) != std::string::npos ||
            lower(group).find(q) != std::string::npos || lower(desc).find(q) != std::string::npos) {
            hits.insert(def.id);
        }
    }
    return hits;
}

} // namespace

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: typing flips to search results, clearing returns to categories",
                 "[widget_catalog][1016][search]") {
    open_catalog();
    REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);
    lv_obj_t* browse = browse_list();
    lv_obj_t* level = search_level();
    lv_obj_t* results = search_results();
    REQUIRE(browse != nullptr);
    REQUIRE(level != nullptr);
    REQUIRE(results != nullptr);

    // Blank box = browse view.
    CHECK_FALSE(lv_obj_has_flag(browse, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(level, LV_OBJ_FLAG_HIDDEN));

    // A query that matches some but not everything: the fixture defaults gate
    // several defs, so "nozzle" (two labels, one description) is a real filter.
    const auto expected = expected_search_hits("nozzle");
    REQUIRE(expected.size() > 0);
    REQUIRE(expected.size() < get_all_widget_defs().size());

    type_query("nozzle");
    CHECK(lv_obj_has_flag(browse, LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(level, LV_OBJ_FLAG_HIDDEN));
    CHECK(visible_rows(results) == expected.size());
    // Rows carry their def ids, so membership is exact, not just a count.
    for (const auto& id : expected) {
        lv_obj_t* row = lv_obj_find_by_name(results, id.c_str());
        INFO("expected hit: " << id);
        REQUIRE(row != nullptr);
        CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));
    }
    // The match count subject drives the empty-result message.
    lv_subject_t* count = lv_xml_get_subject(nullptr, "widget_catalog_match_count");
    REQUIRE(count != nullptr);
    CHECK(lv_subject_get_int(count) == static_cast<int>(expected.size()));

    // Clearing the box restores the category list.
    type_query("");
    CHECK_FALSE(lv_obj_has_flag(browse, LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(level, LV_OBJ_FLAG_HIDDEN));

    force_close();
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: a query hitting nothing shows the empty message",
                 "[widget_catalog][1016][search]") {
    open_catalog();
    REQUIRE(WidgetCatalogOverlay::active_root() != nullptr);
    REQUIRE(expected_search_hits("zzzqxv").empty());

    lv_obj_t* message = search_empty_message();
    REQUIRE(message != nullptr);
    CHECK(lv_obj_has_flag(message, LV_OBJ_FLAG_HIDDEN)); // hidden while browsing

    type_query("zzzqxv");
    lv_obj_t* results = search_results();
    REQUIRE(results != nullptr);
    CHECK(visible_rows(results) == 0);
    // Zero matches is what unhides it — the message itself must be visible.
    CHECK_FALSE(lv_obj_has_flag(message, LV_OBJ_FLAG_HIDDEN));

    // And a query with matches hides it again.
    type_query("nozzle");
    CHECK(visible_rows(results) > 0);
    CHECK(lv_obj_has_flag(message, LV_OBJ_FLAG_HIDDEN));

    force_close();
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: search matches descriptions, not just names",
                 "[widget_catalog][1016][search]") {
    // Pick a word that appears in some def's description but in no def's name —
    // a name-only matcher cannot find it.
    std::string token;
    const PanelWidgetDef* carrier = nullptr;
    for (const auto& def : get_all_widget_defs()) {
        if (!def.description) {
            continue;
        }
        std::string desc = lower(def.description);
        std::string word;
        for (char c : desc + " ") {
            if (std::isalpha(static_cast<unsigned char>(c))) {
                word += c;
            } else {
                if (word.size() >= 5) {
                    bool in_any_name = false;
                    for (const auto& other : get_all_widget_defs()) {
                        if (lower(other.display_name ? other.display_name : other.id).find(word) !=
                            std::string::npos) {
                            in_any_name = true;
                            break;
                        }
                    }
                    if (!in_any_name) {
                        token = word;
                        carrier = &def;
                        break;
                    }
                }
                word.clear();
            }
        }
        if (carrier) {
            break;
        }
    }
    REQUIRE(carrier != nullptr);
    INFO("token '" << token << "' from " << carrier->id << " description");

    open_catalog();
    type_query(token);
    lv_obj_t* results = search_results();
    REQUIRE(results != nullptr);

    const auto expected = expected_search_hits(token);
    REQUIRE(expected.count(carrier->id) == 1); // the description hit is expected
    CHECK(visible_rows(results) == expected.size());
    lv_obj_t* row = lv_obj_find_by_name(results, carrier->id);
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    force_close();
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: search results keep placed widgets dimmed with their badge",
                 "[widget_catalog][1016][search]") {
    // Find a placed, single-instance, ungated widget: its search row must be
    // visible but not tappable, and carry the "Placed" marker.
    const PanelWidgetDef* target = nullptr;
    for (const auto& def : get_all_widget_defs()) {
        if (!def.multi_instance && !def_is_gated_here(def) && widget_config_->is_placed(def.id)) {
            target = &def;
            break;
        }
    }
    REQUIRE(target != nullptr); // proves the placed branch is reachable at all
    INFO("placed widget: " << target->id);

    open_catalog();
    type_query(target->display_name ? target->display_name : target->id);
    lv_obj_t* results = search_results();
    REQUIRE(results != nullptr);
    lv_obj_t* row = lv_obj_find_by_name(results, target->id);
    REQUIRE(row != nullptr);
    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN));

    CHECK_FALSE(lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(row, LV_PART_MAIN) < LV_OPA_COVER);
    std::vector<std::string> labels;
    collect_labels(row, labels);
    CHECK(std::find(labels.begin(), labels.end(), "Placed") != labels.end());

    // Tapping a placed row selects nothing.
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK(selected_ids_.empty());

    force_close();
}

TEST_CASE_METHOD(WidgetCatalogCategoryFixture,
                 "Widget catalog: the Reset action survives the catalog rework",
                 "[widget_catalog][1016]") {
    open_catalog();
    lv_obj_t* root = WidgetCatalogOverlay::active_root();
    REQUIRE(root != nullptr);

    lv_obj_t* reset = lv_obj_find_by_name(root, "action_button");
    REQUIRE(reset != nullptr);
    CHECK_FALSE(lv_obj_has_flag(reset, LV_OBJ_FLAG_HIDDEN));

    // Tapping it opens the reset confirmation, not a silent no-op.
    REQUIRE(ModalStack::instance().empty());
    lv_obj_send_event(reset, LV_EVENT_CLICKED, nullptr);
    settle();
    CHECK_FALSE(ModalStack::instance().empty());

    force_close();
}
