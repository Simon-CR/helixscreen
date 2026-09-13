// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_diag_upload_gate.cpp
 * @brief The diagnostic-upload gate: decision semantics, both behaviours of
 *        the bundle-upload and crash-auto-send chokepoints, and the payload
 *        channel marker (prestonbrown/helixscreen#1410).
 *
 * Every test points the two worker URLs at a loopback CountingWorkerStub via
 * HELIX_BUNDLE_WORKER_URL / HELIX_CRASH_WORKER_URL. libhv honours no proxy
 * environment, so this repointing is the ONLY thing standing between an
 * accidentally ungated run (a reverted gate, a future mutation) and a real
 * upload to crash.helixscreen.org — pipe B would auto-file a live GitHub
 * issue. Do not remove the guards.
 */

#include "ui_update_queue.h"

#include "../helix_test_fixture.h"
#include "netd_test_server.h"
#include "system/crash_history.h"
#include "system/crash_reporter.h"
#include "system/debug_bundle_collector.h"
#include "system/diag_upload_gate.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// A loopback HTTP listener that counts every request served and always
/// answers 200 with the JSON body it was built with — the bundle worker's
/// {"share_code": ...} shape or the crash worker's {"issue_number": ...}
/// shape. "Did anything leave the process" is then a request count.
class CountingWorkerStub {
  public:
    explicit CountingWorkerStub(std::string json_body) : body_(std::move(json_body)) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listen_fd_ >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0; // kernel picks a free port
        REQUIRE(bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(listen(listen_fd_, 4) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        REQUIRE(getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";

        // Bound receive timeout so the accept loop re-checks stop_ instead of
        // blocking forever.
        struct timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000;
        setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        thread_ = std::thread([this] { serve(); });
    }

    ~CountingWorkerStub() {
        stop();
    }

    const std::string& url() const {
        return url_;
    }

    int request_count() const {
        return count_.load();
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }

  private:
    void serve() {
        while (!stopped_) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue; // SO_RCVTIMEO expiry or a spurious wakeup
            }

            // Accepted sockets do not inherit SO_RCVTIMEO on Linux; without
            // this a keep-alive client that sent its request would leave recv
            // blocked forever waiting for bytes that are never coming.
            struct timeval tv {};
            tv.tv_sec = 0;
            tv.tv_usec = 100 * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Read the request head; the body is never parsed.
            std::string req;
            char buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < (1U << 20)) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    break;
                }
                req.append(buf, static_cast<size_t>(n));
            }

            ++count_;

            const std::string http = "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/json\r\n" +
                                     std::string("Content-Length: ") +
                                     std::to_string(body_.size()) +
                                     "\r\n"
                                     "Connection: close\r\n"
                                     "\r\n" +
                                     body_;
            send(fd, http.data(), http.size(), 0);
            close(fd);
        }
    }

    std::string body_;
    std::string url_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<int> count_{0};
    std::atomic<bool> stopped_{false};
};

/// Everything a gating test needs: both worker URLs repointed at counting
/// stubs, the opt-in env pinned to a known state, and the CrashReporter /
/// CrashHistory singletons isolated in a temp directory.
struct DiagUploadGateFixture : public HelixTestFixture {
    DiagUploadGateFixture() {
        temp_dir_ = fs::temp_directory_path() /
                    ("helix_diag_upload_test_" + std::to_string(::getpid()) + "_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir_);

        CrashReporter::instance().shutdown();
        CrashReporter::instance().init(temp_dir_.string());
        helix::CrashHistory::instance().shutdown();
        helix::CrashHistory::instance().init(temp_dir_.string());

        bundle_url_.set(bundle_stub_.url());
        crash_url_.set(crash_stub_.url());
        opt_in_.unset();
    }

    ~DiagUploadGateFixture() {
        CrashReporter::instance().shutdown();
        helix::CrashHistory::instance().shutdown();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    CountingWorkerStub bundle_stub_{"{\"share_code\":\"TESTCODE\"}"};
    CountingWorkerStub crash_stub_{"{\"issue_number\":42,\"issue_url\":\"https://x\"}"};
    helix_test::EnvVarGuard bundle_url_{"HELIX_BUNDLE_WORKER_URL"};
    helix_test::EnvVarGuard crash_url_{"HELIX_CRASH_WORKER_URL"};
    helix_test::EnvVarGuard opt_in_{"HELIX_DIAGNOSTIC_UPLOADS"};
    fs::path temp_dir_;
};

/// Drive the UpdateQueue while waiting for a worker-lane callback to land.
bool settle_done(const std::atomic<bool>& done) {
    return helix_test::wait_until([&done] {
        helix::ui::UpdateQueue::instance().drain();
        return done.load();
    });
}

} // namespace

// ============================================================================
// Decision semantics [diag-uploads]
// ============================================================================

TEST_CASE("diag upload gate: unmarked build defaults off, env overrides both ways",
          "[diag-uploads]") {
    DiagUploadGateFixture fx;

    // The test binary is an unmarked build (plain `make test`), so the
    // compile default is OFF — exactly the state a self-compiled build ships.
    REQUIRE_FALSE(helix::diag::marked_build());
    CHECK_FALSE(helix::diag::uploads_enabled());

    fx.opt_in_.set("1");
    CHECK(helix::diag::uploads_enabled());

    fx.opt_in_.set("0");
    CHECK_FALSE(helix::diag::uploads_enabled());

    // Only exact "1"/"0" are switches; anything else falls back to the
    // compile default (same rule as HELIX_HOT_RELOAD).
    fx.opt_in_.set("yes");
    CHECK_FALSE(helix::diag::uploads_enabled());
}

// ============================================================================
// Pipe A: DebugBundleCollector::upload_async [diag-uploads][debug-bundle]
// ============================================================================

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "upload_async: unmarked build refuses without collecting or uploading",
                 "[diag-uploads][debug-bundle]") {
    bool called = false;
    helix::BundleResult got;

    helix::DebugBundleCollector::upload_async(helix::BundleOptions{},
                                              [&](const helix::BundleResult& r) {
                                                  called = true;
                                                  got = r;
                                              });

    // The refusal is synchronous on the caller's thread — nothing was ever
    // submitted to the worker lane.
    REQUIRE(called);
    CHECK(got.uploads_disabled);
    CHECK_FALSE(got.success);
    CHECK_FALSE(got.error_message.empty());
    CHECK(bundle_stub_.request_count() == 0);
}

TEST_CASE_METHOD(DiagUploadGateFixture,
                 "upload_async: opted-in build uploads through the worker URL",
                 "[diag-uploads][debug-bundle]") {
    opt_in_.set("1");

    std::atomic<bool> done{false};
    helix::BundleResult got;

    helix::DebugBundleCollector::upload_async(helix::BundleOptions{},
                                              [&](const helix::BundleResult& r) {
                                                  got = r;
                                                  done = true;
                                              });

    REQUIRE(settle_done(done));
    CHECK(got.success);
    CHECK(got.share_code == "TESTCODE");
    CHECK(bundle_stub_.request_count() == 1);
}

// ============================================================================
// Pipe B: CrashReporter::try_auto_send [diag-uploads][crash_reporter]
// ============================================================================

TEST_CASE_METHOD(DiagUploadGateFixture, "try_auto_send: unmarked build refuses and falls back",
                 "[diag-uploads][crash_reporter]") {
    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "test";

    CHECK_FALSE(CrashReporter::instance().try_auto_send(report));
    CHECK(crash_stub_.request_count() == 0);
}

TEST_CASE_METHOD(DiagUploadGateFixture, "try_auto_send: opted-in build sends to the worker URL",
                 "[diag-uploads][crash_reporter]") {
    opt_in_.set("1");

    CrashReporter::CrashReport report;
    report.signal = 11;
    report.signal_name = "SIGSEGV";
    report.app_version = "test";

    REQUIRE(CrashReporter::instance().try_auto_send(report));
    CHECK(crash_stub_.request_count() == 1);
}

// ============================================================================
// Payload channel marker [diag-uploads]
// ============================================================================

TEST_CASE("payload marker: bundle and crash report both carry diag_upload_marked",
          "[diag-uploads]") {
    DiagUploadGateFixture fx;

    const json bundle = helix::DebugBundleCollector::collect(helix::BundleOptions{});
    REQUIRE(bundle.contains("diag_upload_marked"));
    CHECK(bundle["diag_upload_marked"] == json(helix::diag::marked_build()));

    CrashReporter::CrashReport report;
    const json report_json = CrashReporter::instance().report_to_json(report);
    REQUIRE(report_json.contains("diag_upload_marked"));
    CHECK(report_json["diag_upload_marked"] == json(helix::diag::marked_build()));
}
