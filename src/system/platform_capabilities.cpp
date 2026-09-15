// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file platform_capabilities.cpp
 * @brief Implementation of hardware capability detection
 *
 * Parses /proc/meminfo and /proc/cpuinfo on Linux systems to detect
 * hardware metrics and classify the platform tier.
 * On macOS, uses sysctl for hardware detection.
 */

#include "platform_capabilities.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <regex>
#include <sstream>

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace helix {

// ============================================================================
// Helper functions
// ============================================================================

namespace {

/**
 * @brief Read entire file content as string
 * @param path File path to read
 * @return File content, or empty string on failure
 */
std::string read_file_content(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

/**
 * @brief Classify hardware metrics into a platform tier
 */
PlatformTier classify_tier(size_t ram_mb, int cores) {
    // EMBEDDED: RAM < 512MB OR single core
    // Note: cores <= 1 includes cores=0 (parse failure) and cores=1 (single core)
    // Both cases should be treated as EMBEDDED for safety
    if (ram_mb < PlatformCapabilities::EMBEDDED_RAM_THRESHOLD_MB || cores <= 1) {
        return PlatformTier::EMBEDDED;
    }

    // STANDARD: RAM >= 768MB AND 4+ cores
    if (ram_mb >= PlatformCapabilities::STANDARD_RAM_THRESHOLD_MB &&
        cores >= PlatformCapabilities::STANDARD_CPU_CORES_MIN) {
        return PlatformTier::STANDARD;
    }

    // Everything else is BASIC
    return PlatformTier::BASIC;
}

/**
 * @brief Set derived capabilities based on tier
 */
void set_derived_capabilities(PlatformCapabilities& caps) {
    switch (caps.tier) {
    case PlatformTier::EMBEDDED:
        // Temp graphs already run 1200 live points on AD5M (EMBEDDED),
        // so a static 132-point frequency response chart is lighter
        caps.supports_charts = true;
        caps.supports_animations = false;
        caps.max_chart_points = 50;
        break;

    case PlatformTier::BASIC:
        caps.supports_charts = true;
        caps.supports_animations = false;
        caps.max_chart_points = PlatformCapabilities::BASIC_CHART_POINTS;
        break;

    case PlatformTier::STANDARD:
        caps.supports_charts = true;
        caps.supports_animations = true;
        caps.max_chart_points = PlatformCapabilities::STANDARD_CHART_POINTS;
        break;
    }
}

// ============================================================================
// macOS-specific detection
// ============================================================================

#ifdef __APPLE__
/**
 * @brief Get total RAM in MB on macOS using sysctl
 * @return Total RAM in MB, or 0 on failure
 */
size_t get_macos_ram_mb() {
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        return static_cast<size_t>(memsize / (1024 * 1024)); // bytes to MB
    }
    return 0;
}

/**
 * @brief Get CPU core count on macOS using sysctl
 * @return Number of CPU cores, or 0 on failure
 */
int get_macos_cpu_cores() {
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0) == 0) {
        return ncpu;
    }
    return 0;
}
#endif

} // namespace

// ============================================================================
// /proc/meminfo parsing
// ============================================================================

size_t parse_meminfo_total_mb(const std::string& content) {
    if (content.empty()) {
        return 0;
    }

    // Look for "MemTotal:" line
    // Format: "MemTotal:        3884136 kB"
    std::regex memtotal_regex(R"(MemTotal:\s+(\d+)\s+kB)");
    std::smatch match;

    if (std::regex_search(content, match, memtotal_regex) && match.size() > 1) {
        try {
            size_t kb = std::stoull(match[1].str());
            return kb / 1024; // Convert kB to MB
        } catch (const std::exception& e) {
            spdlog::warn("Failed to parse MemTotal value: {}", e.what());
            return 0;
        }
    }

    return 0;
}

// ============================================================================
// /proc/cpuinfo parsing
// ============================================================================

CpuInfo parse_cpuinfo(const std::string& content) {
    CpuInfo info;

    if (content.empty()) {
        return info;
    }

    // Count processor entries
    // Each CPU core has a "processor : N" line
    std::regex processor_regex(R"(processor\s*:\s*\d+)");
    auto proc_begin = std::sregex_iterator(content.begin(), content.end(), processor_regex);
    auto proc_end = std::sregex_iterator();
    info.core_count = static_cast<int>(std::distance(proc_begin, proc_end));

    // Extract BogoMIPS (first occurrence)
    // Format: "BogoMIPS : 270.00" or "bogomips : 3999.93"
    std::regex bogomips_regex(R"([Bb]ogo[Mm][Ii][Pp][Ss]\s*:\s*([0-9.]+))");
    std::smatch match;
    if (std::regex_search(content, match, bogomips_regex) && match.size() > 1) {
        try {
            info.bogomips = std::stof(match[1].str());
        } catch (const std::exception&) {
            // Ignore parse failures
        }
    }

    // Extract CPU MHz if BogoMIPS not found or as supplement
    // Format: "cpu MHz : 2400.000"
    std::regex mhz_regex(R"(cpu MHz\s*:\s*([0-9.]+))");
    if (std::regex_search(content, match, mhz_regex) && match.size() > 1) {
        try {
            info.cpu_mhz = static_cast<int>(std::stof(match[1].str()));
        } catch (const std::exception&) {
            // Ignore parse failures
        }
    }

    return info;
}

// ============================================================================
// PlatformCapabilities implementation
// ============================================================================

PlatformCapabilities PlatformCapabilities::detect() {
    PlatformCapabilities caps;

#ifdef __APPLE__
    // macOS: use sysctl for detection
    caps.total_ram_mb = get_macos_ram_mb();
    caps.cpu_cores = get_macos_cpu_cores();
    // bogomips not available on macOS
#else
#ifdef __ANDROID__
    spdlog::debug("Android platform: using /proc for hardware detection");
#endif
    // Linux: read /proc/meminfo
    std::string meminfo_content = read_file_content("/proc/meminfo");
    if (!meminfo_content.empty()) {
        caps.total_ram_mb = parse_meminfo_total_mb(meminfo_content);
    }

    // Read /proc/cpuinfo
    std::string cpuinfo_content = read_file_content("/proc/cpuinfo");
    if (!cpuinfo_content.empty()) {
        CpuInfo cpu_info = parse_cpuinfo(cpuinfo_content);
        caps.cpu_cores = cpu_info.core_count;
        caps.bogomips = cpu_info.bogomips;
    }
#endif

    // Classify tier and set derived capabilities
    caps.tier = classify_tier(caps.total_ram_mb, caps.cpu_cores);
    set_derived_capabilities(caps);

    spdlog::debug("Platform detected: RAM={}MB, cores={}, tier={}", caps.total_ram_mb,
                  caps.cpu_cores, platform_tier_to_string(caps.tier));

    return caps;
}

PlatformCapabilities PlatformCapabilities::from_metrics(size_t ram_mb, int cores,
                                                        float bogomips_val) {
    PlatformCapabilities caps;

    caps.total_ram_mb = ram_mb;
    caps.cpu_cores = cores;
    caps.bogomips = bogomips_val;

    // Classify tier and set derived capabilities
    caps.tier = classify_tier(ram_mb, cores);
    set_derived_capabilities(caps);

    return caps;
}

// ============================================================================
// Utility functions
// ============================================================================

std::string platform_tier_to_string(PlatformTier tier) {
    switch (tier) {
    case PlatformTier::EMBEDDED:
        return "embedded";
    case PlatformTier::BASIC:
        return "basic";
    case PlatformTier::STANDARD:
        return "standard";
    }
    return "unknown";
}

} // namespace helix
