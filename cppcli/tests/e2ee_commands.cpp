// src/e2ee_commands.cpp — E2EE management commands on the vendored desktop
// core (lib/ecore). Moved out of main.cpp during the command refactor.
#include "commands.hpp"
#include "pcore.hpp"
#include "globals.hpp"
#include "core/crypto/verify_controller.hpp"
#include "core/crypto/sas_emojis.hpp"
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace matrixcli;

#define ANSI_BOLD "\033[1m"
#define ANSI_RESET "\033[0m"
#define ANSI_GREEN "\033[32m"

int cmdE2ee(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    auto acct = core.client->account();
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    if (sub == "status" || sub == "info") {
        bool ready = core.sync->decryptor()->isInitialized();
        std::string pickleKey = acct.userId + "/" + acct.deviceId;
        auto rec = core.store->loadOlmAccount(pickleKey);
        int otk = rec ? rec->uploadedKeyCount : 0;
        bool shared = rec ? rec->shared : false;

        if (json_out) {
            nlohmann::json j;
            j["user_id"] = acct.userId;
            j["device_id"] = acct.deviceId;
            j["ready"] = ready;
            j["olm_account_persisted"] = rec.has_value();
            j["one_time_keys_uploaded"] = otk;
            j["account_shared"] = shared;
            std::cout << j.dump() << std::endl;
            return 0;
        }
        std::cout << "E2EE status for " << acct.userId << " (" << acct.deviceId << "):" << std::endl;
        std::cout << "  Decryptor:            " << (ready ? ANSI_GREEN "● ready" ANSI_RESET : "○ not initialized") << std::endl;
        std::cout << "  Olm account:          " << (rec ? "persisted" : "not persisted") << std::endl;
        std::cout << "  One-time keys sent:   " << otk << std::endl;
        std::cout << "  Account shared:       " << (shared ? "yes" : "no") << std::endl;
        return 0;
    }
    if (sub == "upload") {
        core.sync->uploadDeviceKeys(true);
        std::cout << "Device keys upload scheduled." << std::endl;
        return 0;
    }
    if (sub == "fallback") {
        core.sync->uploadFallbackKey();
        std::cout << "Fallback key upload scheduled." << std::endl;
        return 0;
    }
    if (sub == "reset") {
        // Regenerate the device's identity (same device id) + re-upload:
        // repairs sessions whose device-key self-signature is invalid after
        // account resets (peers skip such devices entirely).
        if (!core.sync->decryptor()->resetIdentity()) {
            std::cerr << "Device identity reset failed." << std::endl;
            return 1;
        }
        core.sync->uploadDeviceKeys(true);
        // A fresh fallback key for the NEW identity: the server's fallback
        // still belongs to the old identity — claims on us would return a
        // key our local account can't use (BAD_MESSAGE_KEY_ID on inbound).
        core.sync->uploadFallbackKey();
        // Drop persisted Olm 1:1 sessions: they were created by the OLD
        // identity and carry its (possibly corrupt) sender key — encrypting
        // with them leaks the old key to every message. Peers re-establish
        // sessions via the key-request loop.
        std::string pickleKey = acct.userId + "/" + acct.deviceId;
        core.store->saveOlmSessions("[]", pickleKey);
        std::cout << "Device identity regenerated + keys re-uploaded, Olm sessions "
                     "cleared. Peers will re-share room keys once they see the "
                     "new device keys."
                  << std::endl;
        return 0;
    }
    std::cerr << "Usage: progressive-cli e2ee <status|upload|fallback>" << std::endl;
    return 1;
}

int cmdBackup(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    if (sub == "create") {
        std::string recoveryKey = core.sync->createKeyBackupNow();
        if (recoveryKey.empty()) {
            std::cerr << "Backup creation failed." << std::endl;
            return 1;
        }
        if (json_out) {
            nlohmann::json j;
            j["recovery_key"] = recoveryKey;
            std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Key backup created. SAVE THIS RECOVERY KEY — it is shown once:\n\n"
                      << ANSI_BOLD << recoveryKey << ANSI_RESET << "\n\n";
        }
        return 0;
    }
    if (sub == "upload") {
        bool ok = core.sync->uploadKeyBackupNow();
        if (json_out) {
            nlohmann::json j; j["ok"] = ok; std::cout << j.dump() << std::endl;
        } else {
            std::cout << (ok ? "Backup uploaded." : "Backup upload failed.") << std::endl;
        }
        return ok ? 0 : 1;
    }
    if (sub == "restore") {
        std::string key = args.options.count("recovery-key") ? args.options.at("recovery-key") : (args.positional.size() > 1 ? args.positional[1] : "");
        if (key.empty()) {
            std::cerr << "Usage: progressive-cli backup restore --recovery-key <key>" << std::endl;
            return 1;
        }
        int n = core.sync->restoreKeyBackupNow(key);
        if (json_out) {
            nlohmann::json j; j["sessions_imported"] = n; std::cout << j.dump() << std::endl;
        } else {
            std::cout << "Restored " << n << " sessions from key backup." << std::endl;
        }
        return n > 0 ? 0 : 1;
    }
    if (sub == "delete") {
        bool ok = core.sync->deleteKeyBackupNow();
        if (json_out) {
            nlohmann::json j; j["ok"] = ok; std::cout << j.dump() << std::endl;
        } else {
            std::cout << (ok ? "Key backup deleted." : "Key backup deletion failed.") << std::endl;
        }
        return ok ? 0 : 1;
    }
    std::cerr << "Usage: progressive-cli backup <create|upload|restore|delete>" << std::endl;
    return 1;
}

int cmdCrossSign(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    std::string password = args.options.count("password") ? args.options.at("password") : "";

    auto report = [&](bool ok, const std::string& what) {
        if (json_out) {
            nlohmann::json j; j["ok"] = ok; j["action"] = what; std::cout << j.dump() << std::endl;
        } else {
            std::cout << (ok ? ("Cross-signing " + what + ": OK.") : ("Cross-signing " + what + ": FAILED.")) << std::endl;
        }
        return ok ? 0 : 1;
    };

    if (sub == "setup") {
        if (core.sync->setupCrossSigning()) return report(true, "setup");
        if (!core.sync->uiaSession().empty()) {
            if (password.empty()) {
                if (json_out) {
                    nlohmann::json j; j["ok"] = false; j["need_password"] = true;
                    j["error"] = "server requires password confirmation (UIA) — retry with --password";
                    std::cout << j.dump() << std::endl;
                } else {
                    std::cerr << "Server requires password confirmation — retry with --password." << std::endl;
                }
                return 1;
            }
            if (core.sync->setupCrossSigningWithPassword(password)) return report(true, "setup");
            return report(false, "setup");
        }
        return report(false, "setup");
    }
    if (sub == "reset") {
        if (core.sync->resetCrossSigning()) return report(true, "reset");
        if (!core.sync->uiaSession().empty() && !password.empty())
            if (core.sync->setupCrossSigningWithPassword(password)) return report(true, "reset");
        return report(false, "reset");
    }
    std::cerr << "Usage: progressive-cli crosssign <setup|reset> [--password <pw>]" << std::endl;
    return 1;
}

int cmdSsss(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    std::string key = args.options.count("recovery-key") ? args.options.at("recovery-key") : (args.positional.size() > 1 ? args.positional[1] : "");
    if (key.empty()) {
        std::cerr << "Usage: progressive-cli ssss <upload|retrieve> --recovery-key <key>" << std::endl;
        return 1;
    }

    if (sub == "upload") {
        bool ok = core.sync->uploadSsssSecrets(key);
        if (json_out) {
            nlohmann::json j; j["ok"] = ok; std::cout << j.dump() << std::endl;
        } else {
            std::cout << (ok ? "SSSS secrets uploaded (encrypted to recovery key)." : "SSSS upload failed.") << std::endl;
        }
        return ok ? 0 : 1;
    }
    if (sub == "retrieve") {
        int n = core.sync->retrieveSsssSecrets(key);
        if (json_out) {
            nlohmann::json j; j["result"] = n; std::cout << j.dump() << std::endl;
        } else {
            std::cout << (n > 0 ? "SSSS secrets retrieved and stored locally." : "SSSS retrieval failed (missing secrets or wrong key).") << std::endl;
        }
        return n > 0 ? 0 : 1;
    }
    std::cerr << "Usage: progressive-cli ssss <upload|retrieve> --recovery-key <key>" << std::endl;
    return 1;
}

// SAS device verification (interactive). Requires the sync loop (to-device
// events) — started for the duration of the verification.
// The SAS verification core — shared by the CLI `verify` command and the
// TUI `/verify` slash. The progress lines go to `log`, the emoji match is
// decided by `confirm()` (true = match).
int runSasVerification(const std::string& targetUser,
                       const std::string& targetDevice,
                       int timeoutSec, bool autoConfirm,
                       const std::function<void(const std::string&)>& log,
                       const std::function<bool()>& confirm) {
    auto& core = pcore::core();
    auto acct = core.client->account();

    // E2EE init (olm account + device keys + crypto context) — sendOlmToDevice
    // needs ctxHomeserver_/ctxToken_ set by initializeE2EE; without it every
    // verification reply falls back to PLAIN to-device, which Element rejects.
    std::string e2ee_note = pcore::bootstrap();
    if (!e2ee_note.empty()) {
        log("Warning: " + e2ee_note);
        return 1;
    }

    progressive::desktop::VerificationController controller;
    controller.setClient(core.client);
    controller.setVerificationManager(&core.sync->verificationManager());
    controller.setSyncEngine(core.sync.get());

    pcore::startSync(nullptr);

    if (targetUser == acct.userId) {
        controller.startSelfVerification(acct.userId, acct.deviceId, targetDevice);
    } else {
        controller.startUserVerification(targetUser, targetDevice);
    }

    progressive::desktop::VerificationTransaction* txn = nullptr;
    for (auto* t : core.sync->verificationManager().activeTransactions()) {
        if (t->weInitiated && t->otherUserId == targetUser &&
            t->otherDeviceId == targetDevice && t->state != progressive::desktop::VerificationState::Done) {
            txn = t;
            break;
        }
    }
    if (!txn) {
        log("Verify failed: could not start the verification transaction");
        pcore::stopSync();
        return 1;
    }
    std::string txnId = txn->transactionId;
    auto& vm = core.sync->verificationManager();

    log("Verification started with " + targetUser + "/" + targetDevice
        + " (txn " + txnId.substr(0, 8) + "...) — waiting for the other device...");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool confirmed = false;
    int rc = 1;

    while (matrixcli::g_interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        auto* t = vm.findTransaction(txnId);
        if (!t) { log("Verify failed: transaction gone"); break; }

        if (t->state == progressive::desktop::VerificationState::Cancelled) {
            log("Verification cancelled by the other side ("
                + progressive::desktop::cancelCodeToString(
                    t->cancelCode.value_or(progressive::desktop::CancelCode::Other)) + ")");
            break;
        }
        auto showEmojis = [&](const char* note) {
            auto emojis = vm.computeEmojis(*t);
            if (emojis.empty()) return;
            std::string block = std::string("\n") + note + "\n";
            for (auto& e : emojis) {
                block += "  " + e.emoji + "  " + e.description + "\n";
            }
            log(block);
        };
        if (t->state == progressive::desktop::VerificationState::KeyReceived && !confirmed) {
            showEmojis("Compare these emojis on both devices:");
            if (autoConfirm) {
                confirmed = true;
                controller.confirmMatch(txnId);
                log("Emojis confirmed (--confirm) — sending MAC...");
            } else if (confirm && confirm()) {
                confirmed = true;
                controller.confirmMatch(txnId);
                log("Emojis match confirmed — sending MAC...");
            } else {
                controller.cancelVerification(txnId, "m.user");
                log("Verification cancelled (emojis don't match)");
                break;
            }
        }
        // The other side may confirm FIRST (--confirm / fast client) — its MAC
        // arrives before our own confirmation, skipping KeyReceived. Treat
        // MacReceived the same: show emojis, confirm, send OUR MAC, then the
        // peer's Done (or ours) completes the flow.
        if (t->state == progressive::desktop::VerificationState::MacReceived && !confirmed) {
            showEmojis("Compare these emojis on both devices (peer already confirmed):");
            if (autoConfirm) {
                confirmed = true;
                controller.confirmMatch(txnId);
                log("Emojis confirmed (--confirm) — sending MAC...");
            } else if (confirm && confirm()) {
                confirmed = true;
                controller.confirmMatch(txnId);
                log("Emojis match confirmed — sending MAC...");
            } else {
                controller.cancelVerification(txnId, "m.user");
                log("Verification cancelled (emojis don't match)");
                break;
            }
        }
        if (t->state == progressive::desktop::VerificationState::Done) {
            core.store->saveVerifiedDevice(targetUser, targetDevice);
            log(std::string(ANSI_GREEN "\n  ✓ Device verified: ") + targetUser
                + "/" + targetDevice + ANSI_RESET);
            rc = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (rc != 0 && !confirmed) {
        if (!matrixcli::g_interrupted.load()) {
            log("Verification interrupted (Ctrl+C)");
            controller.cancelVerification(txnId, "m.user");
        } else {
            log("Verification timed out after " + std::to_string(timeoutSec) + "s");
            controller.cancelVerification(txnId, "m.timeout");
        }
    }
    pcore::stopSync();
    return rc;
}

int cmdVerify(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: progressive-cli verify <@user:server> --device <deviceId> [--confirm] [--timeout s] [--json]" << std::endl;
        return 1;
    }
    const std::string targetUser = args.positional[0];
    const std::string targetDevice =
        args.options.count("device") ? args.options.at("device") : "";
    const bool autoConfirm = args.options.count("confirm");
    const bool jsonOut = args.options.count("json");
    int timeoutSec = 120;
    if (args.options.count("timeout")) {
        try { timeoutSec = std::stoi(args.options.at("timeout")); } catch (...) {}
    }
    if (targetDevice.empty()) {
        std::cerr << "Error: --device <deviceId> required" << std::endl;
        return 1;
    }

    // The CLI presentation: the progress on stdout, the emoji match on
    // the terminal (the JSON mode keeps the final record on stdout).
    const int rc = runSasVerification(
        targetUser, targetDevice, timeoutSec, autoConfirm,
        [jsonOut](const std::string& line) {
            if (!jsonOut) std::cout << line << std::endl;
        },
        [jsonOut]() -> bool {
            if (jsonOut) return false;  // no TTY: let it fail/time out
            std::cout << "Do the emojis match? [y/N]: " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            return answer == "y" || answer == "Y" || answer == "yes"
                || answer == "YES";
        });
    if (jsonOut) {
        nlohmann::json j;
        j["ok"] = rc == 0;
        j["user_id"] = targetUser;
        j["device_id"] = targetDevice;
        std::cout << j.dump() << std::endl;
    }
    return rc;
}

// verify-wait: LISTEN for an incoming SAS verification request (initiated by
// another client, e.g. Element's "Verify session") and drive it to completion
// — auto-accept, show the SAS emojis, prompt for confirmation. This is the
// missing inbound half of `verify` (which only initiates).
int cmdVerifyWait(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    bool autoConfirm = args.options.count("confirm");
    bool json_out = args.options.count("json");
    int timeoutSec = 120;
    if (args.options.count("timeout")) {
        try { timeoutSec = std::stoi(args.options.at("timeout")); } catch (...) {}
    }

    auto& core = pcore::core();
    auto acct = core.client->account();

    // E2EE init — the crypto context (homeserver/token) is required for
    // Olm-wrapped verification replies (same reason as cmdVerify).
    std::string e2ee_note = pcore::bootstrap();
    if (!e2ee_note.empty()) {
        std::cerr << "Warning: " << e2ee_note << std::endl;
        return 1;
    }

    progressive::desktop::VerificationController controller;
    controller.setClient(core.client);
    controller.setVerificationManager(&core.sync->verificationManager());
    controller.setSyncEngine(core.sync.get());

    pcore::startSync(nullptr);
    auto& vm = core.sync->verificationManager();

    std::cout << "Listening for incoming verification requests"
              << " (Ctrl+C to cancel, " << timeoutSec << "s timeout)..." << std::endl;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool confirmed = false;
    int rc = 1;
    std::string txnId;

    while (matrixcli::g_interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        // Once a transaction is captured, follow it by id (Done must NOT be
        // filtered out — the completion branch below needs to see it).
        progressive::desktop::VerificationTransaction* incoming = nullptr;
        if (!txnId.empty()) {
            incoming = vm.findTransaction(txnId);
        } else {
            // Find the NEWEST incoming transaction still in flight. The
            // server can re-deliver stale queued copies of old requests
            // (empty-since initial sync replays the to-device queue), so
            // prefer the most recent one — and expire anything older than
            // the SAS spec's 10-minute transaction timeout.
            auto now = std::chrono::steady_clock::now();
            for (auto* t : vm.activeTransactions()) {
                if (t->isIncoming && t->state != progressive::desktop::VerificationState::Done
                    && t->state != progressive::desktop::VerificationState::Cancelled
                    && (now - t->startTime) < std::chrono::minutes(10)) {
                    if (!incoming || t->startTime > incoming->startTime) {
                        incoming = t;
                    }
                }
            }
        }
        if (!incoming) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (txnId.empty()) {
            txnId = incoming->transactionId;
            std::cout << "Incoming verification request from "
                      << incoming->otherUserId << "/" << incoming->otherDeviceId
                      << " (txn " << txnId.substr(0, 8) << "...) — accepting..." << std::endl;
            controller.acceptIncoming(txnId);
        }

        auto* t = vm.findTransaction(txnId);
        if (!t) { std::cerr << "Verify-wait failed: transaction gone" << std::endl; break; }

        if (t->state == progressive::desktop::VerificationState::Cancelled) {
            std::cerr << "Verification cancelled by the other side ("
                      << progressive::desktop::cancelCodeToString(
                             t->cancelCode.value_or(progressive::desktop::CancelCode::Other)) << ")"
                      << std::endl;
            break;
        }
        if (t->state == progressive::desktop::VerificationState::KeyReceived && !confirmed) {
            auto emojis = vm.computeEmojis(*t);
            if (!emojis.empty()) {
                std::cout << "\nCompare these emojis on both devices:\n";
                for (auto& e : emojis) {
                    std::cout << "  " << e.emoji << "  " << e.description << "\n";
                }
                std::cout << std::endl;
            }
            if (autoConfirm) {
                confirmed = true;
                controller.confirmMatch(txnId);
                std::cout << "Emojis confirmed (--confirm) — sending MAC..." << std::endl;
            } else {
                std::cout << "Do the emojis match? [y/N]: " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer == "y" || answer == "Y" || answer == "yes" || answer == "YES") {
                    confirmed = true;
                    controller.confirmMatch(txnId);
                    std::cout << "Emojis match confirmed — sending MAC..." << std::endl;
                } else {
                    controller.cancelVerification(txnId, "m.user");
                    std::cerr << "Verification cancelled (emojis don't match)" << std::endl;
                    break;
                }
            }
        }
        // The other side may confirm FIRST (--confirm / fast client) — its MAC
        // arrives before our own confirmation, skipping KeyReceived. Treat
        // MacReceived the same: show emojis, confirm, send OUR MAC, then the
        // peer's Done (or ours) completes the flow.
        if (t->state == progressive::desktop::VerificationState::MacReceived && !confirmed) {
            auto emojis = vm.computeEmojis(*t);
            if (!emojis.empty()) {
                std::cout << "\nCompare these emojis on both devices (peer already confirmed):\n";
                for (auto& e : emojis) {
                    std::cout << "  " << e.emoji << "  " << e.description << "\n";
                }
                std::cout << std::endl;
            }
            if (autoConfirm) {
                confirmed = true;
                controller.confirmMatch(txnId);
                std::cout << "Emojis confirmed (--confirm) — sending MAC..." << std::endl;
            } else {
                std::cout << "Do the emojis match? [y/N]: " << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer == "y" || answer == "Y" || answer == "yes" || answer == "YES") {
                    confirmed = true;
                    controller.confirmMatch(txnId);
                    std::cout << "Emojis match confirmed — sending MAC..." << std::endl;
                } else {
                    controller.cancelVerification(txnId, "m.user");
                    std::cerr << "Verification cancelled (emojis don't match)" << std::endl;
                    break;
                }
            }
        }
        if (t->state == progressive::desktop::VerificationState::Done) {
            core.store->saveVerifiedDevice(t->otherUserId, t->otherDeviceId);
            if (json_out) {
                nlohmann::json j;
                j["ok"] = true;
                j["user_id"] = t->otherUserId;
                j["device_id"] = t->otherDeviceId;
                std::cout << j.dump() << std::endl;
            } else {
                std::cout << ANSI_GREEN "\n  ✓ Device verified: " << t->otherUserId
                          << "/" << t->otherDeviceId << ANSI_RESET << std::endl;
            }
            rc = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (rc != 0 && !confirmed) {
        if (!matrixcli::g_interrupted.load()) {
            std::cerr << "Verify-wait interrupted (Ctrl+C)" << std::endl;
            if (!txnId.empty()) controller.cancelVerification(txnId, "m.user");
        } else {
            std::cerr << "Verify-wait timed out after " << timeoutSec << "s" << std::endl;
            if (!txnId.empty()) controller.cancelVerification(txnId, "m.timeout");
        }
    }
    pcore::stopSync();
    return rc;
}

// matrixcli passwd --current <pw> --new <pw>   (change the account password)
// matrixcli sessions [--logout <device_id> ...] --password <pw>
int cmdPasswd(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    auto cur = args.options.count("current") ? args.options.at("current") : "";
    auto next = args.options.count("new") ? args.options.at("new") : "";
    if (cur.empty() || next.empty()) {
        std::cerr << "Usage: passwd --current <pw> --new <pw>" << std::endl;
        return 1;
    }
    auto r = core.client->changePassword(cur, next);
    if (r.ok) {
        std::cout << "Password changed. All other devices were logged out by the server." << std::endl;
        return 0;
    }
    std::cerr << "Change password failed (HTTP " << r.httpStatus << "): " << r.error.message
              << std::endl;
    return 1;
}

int cmdSessions(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    auto resp = core.client->listDevices();
    if (!resp.ok) {
        std::cerr << "Could not fetch sessions (HTTP " << resp.httpStatus << ")" << std::endl;
        return 1;
    }
    std::string ourDeviceId = core.client->account().deviceId;
    std::vector<std::string> ids;
    {
        simdjson::dom::parser p;
        auto doc = p.parse(resp.data);
        if (doc.error() == simdjson::SUCCESS) {
            auto devs = doc.value()["devices"];
            for (auto dev : devs.get_array().value()) {
                std::string id, dn;
                auto idv = dev["device_id"].get_string();
                if (idv.error() == simdjson::SUCCESS) id = std::string(idv.value());
                auto dnv = dev["display_name"].get_string();
                if (dnv.error() == simdjson::SUCCESS) dn = std::string(dnv.value());
                if (dn.empty()) dn = id;
                std::cout << (id == ourDeviceId ? "[current] " : "          ")
                          << id << "  " << dn << std::endl;
                if (id != ourDeviceId) ids.push_back(id);
            }
        }
    }
    if (args.options.count("logout")) {
        auto pw = args.options.count("password") ? args.options.at("password") : "";
        if (pw.empty()) { std::cerr << "Session logout needs --password <pw>" << std::endl; return 1; }
        std::string to = args.options.at("logout");
        if (to == "others") {
            for (const auto& id : ids) {
                auto r = core.client->deleteDevice(id, pw);
                std::cout << "logout " << id << ": " << (r.ok ? "ok" : "FAILED") << std::endl;
            }
        } else {
            auto r = core.client->deleteDevice(to, pw);
            std::cout << "logout " << to << ": " << (r.ok ? "ok" : "FAILED (HTTP " +
                std::to_string(r.httpStatus) + ")") << std::endl;
        }
    }
    return 0;
}

void registerE2eeCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("e2ee", cmdE2ee, "E2EE status and key management (status/upload/fallback/reset)");
    reg.registerCli("backup", cmdBackup, "Key backup: create/upload/restore/delete (--recovery-key)");
    reg.registerCli("crosssign", cmdCrossSign, "Cross-signing: setup/reset (--password for UIA)");
    reg.registerCli("ssss", cmdSsss, "Secret storage: upload/retrieve (--recovery-key)");
    reg.registerCli("verify", cmdVerify, "SAS-verify a device: verify <user> --device <id> [--confirm]");
    reg.registerCli("verify-wait", cmdVerifyWait,
        "Accept an incoming SAS request: verify-wait [--confirm] [--timeout s]");
    reg.registerCli("passwd", cmdPasswd, "Change the account password: passwd --current <pw> --new <pw>");
    reg.registerCli("sessions", cmdSessions, "List sessions: sessions [--logout <id|others> --password <pw>]");
}
