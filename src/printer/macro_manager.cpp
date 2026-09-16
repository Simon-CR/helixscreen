// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macro_manager.h"

#include "data_root_resolver.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>

namespace helix {

// ============================================================================
// File Loading Helpers
// ============================================================================

namespace {

/**
 * @brief Load macro content from config file
 * @return File content, or empty string if not found
 *
 * Resolves via find_readable: writable config dir (user override), then
 * shipped seed under HELIX_DATA_DIR/assets/config/. Falls back to
 * /opt/helixscreen/config/ for legacy AD5M Klipper-mod installs.
 */
std::string load_macro_file() {
    const std::vector<std::string> paths = {
        helix::find_readable("helix_macros.cfg"),
        "/opt/helixscreen/config/helix_macros.cfg", // Legacy AD5M install
    };

    for (const auto& path : paths) {
        std::ifstream file(path);
        if (file) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            spdlog::debug("[MacroManager] Loaded macro file from {}", path);
            return buffer.str();
        }
    }

    spdlog::warn("[MacroManager] Could not find helix_macros.cfg in any expected location");
    return "";
}

/**
 * @brief Parse version from file header comment
 * @param content File content
 * @return Version string (e.g., "2.0.0"), or empty if not found
 *
 * Looks for pattern: # helix_macros v<version>
 */
std::string parse_file_version(const std::string& content) {
    static const std::regex version_pattern(R"(#\s*helix_macros\s+v(\d+\.\d+\.\d+))");
    std::smatch match;
    if (std::regex_search(content, match, version_pattern)) {
        return match[1].str();
    }
    return "";
}

/**
 * @brief Parse macro names from file content
 * @param content File content
 * @return Vector of macro names
 */
std::vector<std::string> parse_macro_names(const std::string& content) {
    std::vector<std::string> names;
    // Anchored to line start: a real Klipper section header always starts in
    // column 0, so this does not also match one quoted inside a comment.
    static const std::regex macro_pattern(R"(^\[gcode_macro\s+(\w+)\])");

    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, macro_pattern)) {
            std::string name = match[1].str();
            // Skip internal state macros
            if (!name.empty() && name[0] != '_') {
                names.push_back(name);
            }
        }
    }

    return names;
}

/**
 * @brief Infer the installed pack version from which rung macros exist
 * @param hardware Discovery snapshot with the printer's macro names
 *
 * The installed pack exposes no queryable version, so infer it from which
 * macros exist - each rung is the newest macro whose presence brackets the
 * install. Adding a macro to the pack means adding a rung here (and a
 * matching version bump in helix_macros.cfg).
 */
std::optional<std::string> parse_installed_version(const PrinterDiscovery& hardware) {
    if (hardware.has_helix_macro("HELIX_UNLOAD_FILAMENT")) {
        return "2.1.0";
    }

    if (hardware.has_helix_macro("HELIX_READY")) {
        return "2.0.0";
    }

    // Check for legacy v1.x macros
    if (hardware.has_helix_macro("HELIX_START_PRINT")) {
        return "1.0.0";
    }

    return std::nullopt;
}

/**
 * @brief Timestamped sibling name for a printer.cfg backup
 *
 * The upload overwrites printer.cfg wholesale, so the pre-edit content must
 * be recoverable from the printer itself; a timestamp keeps every install's
 * backup distinct.
 */
std::string printer_cfg_backup_name() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[24] = {};
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_buf);
    return std::string("printer.cfg.helixbak-") + stamp;
}

} // anonymous namespace

// ============================================================================
// MacroManager Implementation
// ============================================================================

MacroManager::MacroManager(IMoonrakerAPI& api, const PrinterDiscovery& hardware)
    : api_(api), hardware_(hardware) {}

MacroManager::~MacroManager() = default;

bool MacroManager::is_installed() const {
    return hardware_.has_helix_macros();
}

MacroInstallStatus MacroManager::get_status() const {
    return evaluate_status(hardware_);
}

MacroInstallStatus MacroManager::evaluate_status(const PrinterDiscovery& hardware) {
    // No objects list consumed yet: the printer has not told us anything,
    // and "no helix macros found" in that vacuum would be a claim, not an
    // observation.
    if (!hardware.objects_reported()) {
        return MacroInstallStatus::UNKNOWN;
    }

    if (!hardware.has_helix_macros()) {
        return MacroInstallStatus::NOT_INSTALLED;
    }

    auto installed_version = parse_installed_version(hardware);
    if (!installed_version) {
        // Has macros but can't determine version - assume installed
        return MacroInstallStatus::INSTALLED;
    }

    // Compare against version from local file
    std::string local_version = get_version();
    if (local_version.empty()) {
        // Can't read local file - assume installed
        return MacroInstallStatus::INSTALLED;
    }

    if (version_less(*installed_version, local_version)) {
        return MacroInstallStatus::OUTDATED;
    }

    return MacroInstallStatus::INSTALLED;
}

bool MacroManager::version_less(const std::string& a, const std::string& b) {
    auto next_component = [](const std::string& v, size_t& pos) {
        if (pos == std::string::npos) {
            return 0L;
        }
        size_t end = v.find('.', pos);
        std::string part = v.substr(pos, end == std::string::npos ? end : end - pos);
        pos = end == std::string::npos ? end : end + 1;
        // Non-numeric components fall back to 0 rather than aborting the parse
        long value = 0;
        try {
            value = std::stol(part);
        } catch (const std::exception&) {
        }
        return value;
    };

    size_t pa = 0, pb = 0;
    while (pa != std::string::npos || pb != std::string::npos) {
        long ca = next_component(a, pa);
        long cb = next_component(b, pb);
        if (ca != cb) {
            return ca < cb;
        }
    }
    return false;
}

std::string MacroManager::get_installed_version() const {
    auto version = parse_installed_version(hardware_);
    return version.value_or("");
}

bool MacroManager::update_available() const {
    return get_status() == MacroInstallStatus::OUTDATED;
}

void MacroManager::install_files(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Staging macro files...");

    // upload_macro_file() lands both callbacks on the main thread, so this
    // continuation runs there. Every chain that follows hops its own terminal
    // callbacks to main too (see the queue pin test), because the caller's
    // continuation touches LVGL-adjacent state.
    upload_macro_file(
        [this, on_success, on_error]() {
            spdlog::info("[HelixMacroManager] Macro file uploaded, adding include...");
            // Step 2: Back up printer.cfg and add the include
            add_include_to_config(on_success, on_error);
        },
        on_error);
}

void MacroManager::update_files(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Staging macro update (file upload only)...");

    upload_macro_file(on_success, on_error);
}

void MacroManager::request_restart(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Requesting Klipper restart...");

    // printer.restart completes on the WebSocket loop; the caller's
    // continuation touches LVGL-adjacent state, so hop it to main.
    api_.restart_klipper(
        lifetime_.bg_cb("MacroManager::restart_done",
                        [on_success]() {
                            spdlog::info("[HelixMacroManager] Klipper restart initiated");
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb("MacroManager::restart_failed", [on_error](const MoonrakerError& err) {
            spdlog::error("[HelixMacroManager] Klipper restart "
                          "request failed: {}",
                          err.message);
            if (on_error) {
                on_error(err);
            }
        }));
}

void MacroManager::uninstall(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Starting macro uninstall...");

    auto token = lifetime_.token();

    // Step 1: Remove include from printer.cfg
    remove_include_from_config(
        [this, token, on_success, on_error]() {
            // L081 Mechanism C: defer chained this-> work to main thread
            // (remove_include cb fires on HTTP bg thread).
            token.defer("MacroManager::uninstall_step2", [this, token, on_success, on_error]() {
                // Step 2: Delete macro file
                delete_macro_file(
                    [this, token, on_success, on_error]() {
                        token.defer("MacroManager::uninstall_step3",
                                    [this, on_success, on_error]() {
                                        // Step 3: Restart Klipper
                                        request_restart(on_success, on_error);
                                    });
                    },
                    on_error);
            });
        },
        on_error);
}

std::string MacroManager::get_macro_content() {
    return load_macro_file();
}

std::string MacroManager::get_version() {
    std::string content = load_macro_file();
    return parse_file_version(content);
}

std::vector<std::string> MacroManager::get_macro_names() {
    std::string content = load_macro_file();
    if (content.empty()) {
        return {};
    }
    return parse_macro_names(content);
}

// ============================================================================
// Private Implementation
// ============================================================================

void MacroManager::upload_macro_file(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Uploading {} to printer config directory",
                 HELIX_MACROS_FILENAME);

    // Get the macro content to upload
    std::string content = get_macro_content();

    spdlog::debug("[HelixMacroManager] Macro content size: {} bytes", content.size());

    // Upload to config root (not gcodes)
    // The path is "" because we upload directly to the config directory.
    // Completion arrives on the HttpExecutor lane; every continuation in the
    // install/update chains touches api_ state or LVGL-adjacent subjects, so
    // both callbacks hop to the main thread here.
    api_.transfers().upload_file_with_name(
        "config", "", HELIX_MACROS_FILENAME, content,
        lifetime_.bg_cb("MacroManager::upload_done",
                        [on_success]() {
                            spdlog::info("[HelixMacroManager] Successfully uploaded {}",
                                         HELIX_MACROS_FILENAME);
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb("MacroManager::upload_failed", [on_error](const MoonrakerError& err) {
            spdlog::error("[HelixMacroManager] Failed to upload {}: {}", HELIX_MACROS_FILENAME,
                          err.message);
            if (on_error) {
                on_error(err);
            }
        }));
}

void MacroManager::add_include_to_config(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Adding include line to printer.cfg");

    auto token = lifetime_.token();

    // Download printer.cfg
    api_.transfers().download_file(
        "config", "printer.cfg",
        // Download success — runs on HTTP bg thread.
        // L081 Mechanism C: do pure parsing/construction locally on BG, then defer
        // the api_-> kick-off to main thread.
        [this, token, on_success, on_error](const std::string& content) {
            // Check if include line already exists
            std::string include_line = "[include " + std::string(HELIX_MACROS_FILENAME) + "]";
            if (content.find(include_line) != std::string::npos) {
                spdlog::info("[HelixMacroManager] Include line already present in printer.cfg");
                token.defer("MacroManager::include_already_present", [on_success]() {
                    if (on_success) {
                        on_success();
                    }
                });
                return;
            }

            // Find the best place to insert the include line
            // Strategy: Insert after the last existing [include ...] line, or at the very top
            std::string modified_content;
            std::istringstream input(content);
            std::string line;
            size_t last_include_end = 0;
            size_t current_pos = 0;

            // First pass: find the position after the last [include] line
            while (std::getline(input, line)) {
                current_pos += line.length() + 1; // +1 for newline
                // Check for [include ...] pattern (case-insensitive for robustness)
                std::string lower_line = line;
                std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (lower_line.find("[include ") == 0 || lower_line.find("[include\t") == 0) {
                    last_include_end = current_pos;
                }
            }

            // Second pass: insert at the right position
            if (last_include_end > 0) {
                // Insert after last include line
                modified_content = content.substr(0, last_include_end) + include_line + "\n" +
                                   content.substr(last_include_end);
                spdlog::debug("[HelixMacroManager] Inserted after existing includes at pos {}",
                              last_include_end);
            } else {
                // No existing includes - add at the very beginning
                modified_content = include_line + "\n" + content;
                spdlog::debug("[HelixMacroManager] Inserted at beginning of file");
            }

            // Defer the upload kick-off to main thread (needs api_)
            const std::string backup_name = printer_cfg_backup_name();
            token.defer(
                "MacroManager::add_include_upload",
                [this, on_success, on_error, backup_name, original_content = content,
                 modified_content = std::move(modified_content)]() mutable {
                    // Back up the original first: if the backup upload
                    // fails, the overwrite must not happen. Its
                    // completion fires on the HTTP lane, so the
                    // follow-up upload kick-off hops back to main.
                    api_.transfers().upload_file_with_name(
                        "config", "", backup_name, original_content,
                        lifetime_.bg_cb(
                            "MacroManager::backup_done",
                            [this, on_success, on_error, backup_name,
                             modified_content = std::move(modified_content)]() mutable {
                                spdlog::info("[HelixMacroManager] Backed up printer.cfg "
                                             "to {}",
                                             backup_name);
                                api_.transfers().upload_file_with_name(
                                    "config", "", "printer.cfg", modified_content,
                                    lifetime_.bg_cb(
                                        "MacroManager::add_include_done",
                                        [on_success]() {
                                            spdlog::info("[HelixMacroManager] Successfully added "
                                                         "include to printer.cfg");
                                            if (on_success) {
                                                on_success();
                                            }
                                        }),
                                    lifetime_.bg_cb("MacroManager::add_include_failed",
                                                    [on_error](const MoonrakerError& err) {
                                                        spdlog::error(
                                                            "[HelixMacroManager] Failed to upload "
                                                            "modified printer.cfg: {}",
                                                            err.message);
                                                        if (on_error) {
                                                            on_error(err);
                                                        }
                                                    }));
                            }),
                        lifetime_.bg_cb("MacroManager::backup_failed",
                                        [on_error, backup_name](const MoonrakerError& err) {
                                            spdlog::error("[HelixMacroManager] Backup upload of {} "
                                                          "failed - refusing to overwrite "
                                                          "printer.cfg: {}",
                                                          backup_name, err.message);
                                            if (on_error) {
                                                on_error(err);
                                            }
                                        }));
                });
        },
        // Download error — fires on the HTTP lane; hop the caller's continuation.
        lifetime_.bg_cb("MacroManager::download_failed", [on_error](const MoonrakerError& err) {
            spdlog::error("[HelixMacroManager] Failed to download printer.cfg: {}", err.message);
            if (on_error) {
                on_error(err);
            }
        }));
}

void MacroManager::remove_include_from_config(SuccessCallback on_success, ErrorCallback on_error) {
    spdlog::info("[HelixMacroManager] Removing include line from printer.cfg");

    auto token = lifetime_.token();

    // Download printer.cfg
    api_.transfers().download_file(
        "config", "printer.cfg",
        // Download success — runs on HTTP bg thread.
        // L081 Mechanism C: do pure parsing/construction locally on BG, then defer
        // the api_-> kick-off to main thread.
        [this, token, on_success, on_error](const std::string& content) {
            std::string include_line = "[include " + std::string(HELIX_MACROS_FILENAME) + "]";

            // Check if include line exists
            size_t pos = content.find(include_line);
            if (pos == std::string::npos) {
                spdlog::info("[HelixMacroManager] Include line not found in printer.cfg");
                token.defer("MacroManager::include_not_found", [on_success]() {
                    if (on_success) {
                        on_success();
                    }
                });
                return;
            }

            // Find the full line to remove (including newline)
            size_t line_start = pos;
            size_t line_end = content.find('\n', pos);
            if (line_end == std::string::npos) {
                line_end = content.length();
            } else {
                line_end++; // Include the newline
            }

            // Build modified content without the include line
            std::string modified_content = content.substr(0, line_start) + content.substr(line_end);

            spdlog::debug("[HelixMacroManager] Removed include line at pos {}-{}", line_start,
                          line_end);

            // Defer the upload kick-off to main thread (needs api_)
            token.defer("MacroManager::remove_include_upload", [this, on_success, on_error,
                                                                modified_content = std::move(
                                                                    modified_content)]() mutable {
                // Upload modified printer.cfg
                api_.transfers().upload_file_with_name(
                    "config", "", "printer.cfg", modified_content,
                    lifetime_.bg_cb("MacroManager::remove_include_done",
                                    [on_success]() {
                                        spdlog::info("[HelixMacroManager] Successfully removed "
                                                     "include from printer.cfg");
                                        if (on_success) {
                                            on_success();
                                        }
                                    }),
                    lifetime_.bg_cb("MacroManager::remove_include_failed",
                                    [on_error](const MoonrakerError& err) {
                                        spdlog::error(
                                            "[HelixMacroManager] Failed to upload modified "
                                            "printer.cfg: {}",
                                            err.message);
                                        if (on_error) {
                                            on_error(err);
                                        }
                                    }));
            });
        },
        // Download error - fires on the HTTP lane; hop the caller's continuation.
        lifetime_.bg_cb("MacroManager::remove_include_download_failed",
                        [on_error](const MoonrakerError& err) {
                            spdlog::error("[HelixMacroManager] Failed to download printer.cfg: {}",
                                          err.message);
                            if (on_error) {
                                on_error(err);
                            }
                        }));
}

void MacroManager::delete_macro_file(SuccessCallback on_success, ErrorCallback on_error) {
    // Use IMoonrakerAPI to delete the file. Both completions hop to main like
    // every other terminal callback in this class.
    api_.files().delete_file(
        std::string("config/") + HELIX_MACROS_FILENAME,
        lifetime_.bg_cb("MacroManager::delete_done",
                        [on_success]() {
                            if (on_success) {
                                on_success();
                            }
                        }),
        lifetime_.bg_cb("MacroManager::delete_failed",
                        [on_success, on_error](const MoonrakerError& err) {
                            // File might not exist - that's OK for uninstall
                            if (err.type == MoonrakerErrorType::FILE_NOT_FOUND) {
                                spdlog::debug("[HelixMacroManager] Macro file already deleted");
                                if (on_success) {
                                    on_success(); // Continue with success path
                                }
                            } else if (on_error) {
                                on_error(err);
                            }
                        }));
}

} // namespace helix
