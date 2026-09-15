// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_job_queue_start_guard.cpp
 * @brief Starting a queued job must not delete the entry it then fails to start.
 *
 * Run with: ./build/bin/helix-tests "[job_queue][print_state]"
 *
 * JobQueueModal::start_job() removes the entry from Moonraker's queue FIRST and
 * starts the print from the removal's success callback. That ordering is only
 * safe while the refusal in front of it is complete. It was not: the guard read
 * print_stats.state, which reports standby for the whole of a host-side
 * pre-print block, so a tap during that window deleted the queue entry and then
 * ran into PrintStartController's own refusal - the job was gone with nothing
 * printing and nothing queued.
 *
 * can_start_new_print() is the predicate that covers both axes: the printer's
 * reported state AND the app's committed-but-unconfirmed start.
 *
 * The second guard here is the printer-stopping command check: a queued entry
 * reaches the queue from a slicer or web UI, so nothing in the app has ever
 * scanned it.
 */

#include "ui_job_queue_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/job_queue_modal_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../ui_test_utils.h"
#include "app_globals.h"
#include "macro_param_cache.h"
#include "moonraker_api.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "test_helpers/printer_state_test_access.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::JobQueueModal;
using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

/// Records every JSON-RPC method the modal sends, so "did it touch the queue?"
/// is answered by observing the wire rather than by a spy on our own code.
class RecordingClient : public MoonrakerClientMock {
  public:
    RecordingClient() : MoonrakerClientMock(MoonrakerClientMock::PrinterType::VORON_24) {}

    helix::RequestId send_jsonrpc(
        const std::string& method, const json& params, std::function<void(const json&)> success_cb,
        std::function<void(const MoonrakerError&)> error_cb, uint32_t timeout_ms = 0,
        bool silent = false,
        std::optional<helix::rpc_error_policy::CallerIntent> intent = std::nullopt) override {
        methods.push_back(method);
        return MoonrakerClientMock::send_jsonrpc(method, params, std::move(success_cb),
                                                 std::move(error_cb), timeout_ms, silent, intent);
    }

    [[nodiscard]] int count(const std::string& method) const {
        int n = 0;
        for (const auto& m : methods) {
            if (m == method) {
                ++n;
            }
        }
        return n;
    }

    std::vector<std::string> methods;
};

constexpr const char* DELETE_JOB = "server.job_queue.delete_job";

/// Counts the file-head reads the start path makes, so "it blocked" can be told
/// apart from "it never looked".
class CountingTransfers : public MoonrakerFileTransferAPIMock {
  public:
    using MoonrakerFileTransferAPIMock::MoonrakerFileTransferAPIMock;

    void download_file_partial(const std::string& root, const std::string& path, size_t max_bytes,
                               StringCallback on_success, ErrorCallback on_error) override {
        ++partial_reads;
        MoonrakerFileTransferAPIMock::download_file_partial(
            root, path, max_bytes, std::move(on_success), std::move(on_error));
    }

    int partial_reads = 0;
};

/// The real API everywhere except the file reads, which come from disk.
class TransferMockAPI : public MoonrakerAPI {
  public:
    TransferMockAPI(helix::MoonrakerClient& client, helix::PrinterState& state,
                    CountingTransfers& transfers)
        : MoonrakerAPI(client, state), transfers_(transfers) {}

    MoonrakerFileTransferAPI& transfers() override {
        return transfers_;
    }

  private:
    CountingTransfers& transfers_;
};

/// A .gcode in the directory the transfer mock resolves names from.
class PlantedAsset {
  public:
    PlantedAsset(const std::string& name, const std::string& content) {
        for (const auto* prefix : {"", "../", "../../"}) {
            const std::string dir = std::string(prefix) + "assets/test_gcodes";
            if (std::filesystem::is_directory(dir)) {
                path_ = dir + "/" + name;
                break;
            }
        }
        REQUIRE_FALSE(path_.empty());
        std::ofstream(path_, std::ios::trunc) << content;
    }
    ~PlantedAsset() {
        std::remove(path_.c_str());
    }
    PlantedAsset(const PlantedAsset&) = delete;
    PlantedAsset& operator=(const PlantedAsset&) = delete;

  private:
    std::string path_;
};

/// This printer's M729 is a macro that shuts it down.
class StopMacroCache {
  public:
    StopMacroCache() {
        nlohmann::json config;
        config["gcode_macro M729"] = {
            {"gcode", "{action_emergency_stop(\"M729 is not supported\")}"}};
        helix::MacroParamCache::instance().populate_from_configfile(config, {"M729"});
    }
    ~StopMacroCache() {
        helix::MacroParamCache::instance().clear();
    }
    StopMacroCache(const StopMacroCache&) = delete;
    StopMacroCache& operator=(const StopMacroCache&) = delete;
};

class JobQueueStartFixture : public LVGLTestFixture {
  public:
    JobQueueStartFixture() {
        // The global PrinterState is shared across the shard: reset and
        // re-init, or a prior case's subjects decide this one's answers.
        auto& ps0 = get_printer_state();
        PrinterStateTestAccess::reset(ps0);
        ps0.init_subjects(false);
        client.connect("ws://mock/websocket", []() {}, []() {});
        api = std::make_unique<TransferMockAPI>(client, get_printer_state(), transfers);
        previous_api_ = get_moonraker_api();
        set_moonraker_api(api.get());
        // A prior case's preparing job would refuse every start here.
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        settle();
    }

    ~JobQueueStartFixture() override {
        auto& ps = get_printer_state();
        if (ps.has_preparing_job()) {
            ps.retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        settle();
        set_moonraker_api(previous_api_);
        api.reset();
        client.stop_temperature_simulation();
        client.disconnect();
    }

    /// One drain is not enough: a handler running during a drain queues more
    /// work that is still pending when drain() returns.
    static void settle() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    RecordingClient client;
    CountingTransfers transfers{client, "http://mock"};
    std::unique_ptr<MoonrakerAPI> api;

  private:
    IMoonrakerAPI* previous_api_ = nullptr;
};

} // namespace

TEST_CASE_METHOD(JobQueueStartFixture, "JobQueueModal starts a queued job when the printer is idle",
                 "[job_queue][print_state]") {
    // The baseline that stops every refusal assertion below from passing
    // vacuously: with nothing running, the queue entry IS removed.
    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 1);
}

TEST_CASE_METHOD(JobQueueStartFixture, "JobQueueModal refuses to start while a print is running",
                 "[job_queue][print_state]") {
    // Characterization of the behaviour that already worked - the wire is
    // enough here, and it must stay refused after the predicate changes.
    auto& ps = get_printer_state();

    SECTION("printing") {
        set_wire_state(ps, PrintJobState::PRINTING);
    }
    SECTION("paused") {
        set_wire_state(ps, PrintJobState::PAUSED);
    }
    settle();

    // The refusal is answered: the row tap must not look ignored.
    std::vector<std::string> warnings;
    helix::ui::set_test_notification_warning_hook(
        [&warnings](const std::string& message) { warnings.push_back(message); });
    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();
    helix::ui::set_test_notification_warning_hook(nullptr);

    CHECK(client.count(DELETE_JOB) == 0);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("benchy.gcode") != std::string::npos);
}

TEST_CASE_METHOD(JobQueueStartFixture,
                 "JobQueueModal refuses to start during a host-side pre-print block",
                 "[job_queue][print_state]") {
    // THE BUG. The app has committed to a print and is running the user's
    // pre-start block itself, so print_stats.state still reads standby. Under
    // the old wire-only guard the entry was deleted here and the start then
    // failed, losing the job.
    auto& ps = get_printer_state();
    ps.begin_preparing(helix::PrintJobRef{"queued.gcode", "", ""});
    settle();
    REQUIRE(ps.get_print_job_state() == PrintJobState::STANDBY);
    REQUIRE(ps.is_print_in_progress());

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "benchy.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 0);
}

TEST_CASE_METHOD(JobQueueStartFixture, "JobQueueModal refuses a queued file that stops the printer",
                 "[job_queue][printer_stop]") {
    // Nothing in the app enqueues jobs, so a queue entry has never been through
    // the detail view's scan: this start path is the only place it is read.
    StopMacroCache macros;
    PlantedAsset file("queued_stop.gcode", "G28\nM729\nG1 X10 Y10 E1\n");

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "queued_stop.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 0);
    // An absence assertion has to prove the code ran.
    CHECK(transfers.partial_reads == 1);
}

TEST_CASE_METHOD(JobQueueStartFixture,
                 "JobQueueModal starts a queued file that calls no such macro",
                 "[job_queue][printer_stop]") {
    // Stops the refusal above from passing vacuously.
    StopMacroCache macros;
    PlantedAsset file("queued_clean.gcode", "G28\nG1 X10 Y10 E1\n");

    JobQueueModal modal;
    JobQueueModalTestAccess::start_job(modal, "job-1", "queued_clean.gcode");
    settle();

    CHECK(client.count(DELETE_JOB) == 1);
    CHECK(transfers.partial_reads == 1);
}
