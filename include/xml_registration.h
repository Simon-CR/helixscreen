// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file xml_registration.h
 * @brief XML component registration in correct dependency order
 *
 * @threading Main thread only; must complete before UI creation
 * @note Fonts and images are registered via AssetManager::register_fonts/images()
 */

#pragma once

namespace helix {

/**
 * @brief Register XML components from ui_xml/ directory
 *
 * Registers all XML component definitions in dependency order.
 * Must be called after AssetManager initialization and theme init.
 */
void register_xml_components();

/**
 * @brief Register the filament_catalog_selector family of XML components
 *
 * filament_catalog_row / filament_catalog_add_row / filament_catalog_empty_row
 * are never named in any XML markup: FilamentCatalogSelector::rebuild_product_list()
 * creates them by name at runtime, so a caller that never invokes this leaves
 * lv_xml_create() silently returning nullptr for those names. Shared between
 * register_xml_components() and XMLTestFixture::setup_global_xml_registrations_once()
 * so a component added to the set cannot be forgotten in one of the two.
 */
void register_filament_catalog_components();

/**
 * @brief Deinitialize XML-related subjects
 *
 * Must be called during shutdown before lv_deinit().
 * Called by StaticPanelRegistry.
 */
void deinit_xml_subjects();

} // namespace helix
