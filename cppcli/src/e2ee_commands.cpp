// src/e2ee_commands.cpp — E2EE management commands on the vendored desktop
// core (lib/ecore). Moved out of main.cpp during the command refactor.
#include "commands.hpp"
#include "pcore.hpp"
#include "globals.hpp"
#include "core/crypto/verify_controller.hpp"
#include "core/crypto/sas_emojis.hpp"
#include <nlohmann/json.hpp>
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
    std::cerr << "Usage: matrixcli e2ee <status|upload|fallback>" << std::endl;
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
            std::cerr << "Usage: matrixcli backup restore --recovery-key <key>" << std::endl;
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
    std::cerr << "Usage: matrixcli backup <create|upload|restore|delete>" << std::endl;
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
    std::cerr << "Usage: matrixcli crosssign <setup|reset> [--password <pw>]" << std::endl;
    return 1;
}

int cmdSsss(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    auto& core = pcore::core();
    std::string sub = args.positional.empty() ? "status" : args.positional[0];
    bool json_out = args.options.count("json");

    std::string key = args.options.count("recovery-key") ? args.options.at("recovery-key") : (args.positional.size() > 1 ? args.positional[1] : "");
    if (key.empty()) {
        std::cerr << "Usage: matrixcli ssss <upload|retrieve> --recovery-key <key>" << std::endl;
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
    std::cerr << "Usage: matrixcli ssss <upload|retrieve> --recovery-key <key>" << std::endl;
    return 1;
}

// SAS device verification (interactive). Requires the sync loop (to-device
// events) — started for the duration of the verification.
int cmdVerify(const cli::Args& args) {
    if (!pcore::requireSession()) return 1;
    if (args.positional.empty()) {
        std::cerr << "Usage: matrixcli verify <@user:server> --device <deviceId> [--confirm] [--timeout s] [--json]" << std::endl;
        return 1;
    }

    auto& core = pcore::core();
    auto acct = core.client->account();
    std::string targetUser = args.positional[0];
    std::string targetDevice = args.options.count("device") ? args.options.at("device") : "";
    bool autoConfirm = args.options.count("confirm");
    bool json_out = args.options.count("json");
    int timeoutSec = 120;
    if (args.options.count("timeout")) {
        try { timeoutSec = std::stoi(args.options.at("timeout")); } catch (...) {}
    }

    if (targetDevice.empty()) {
        std::cerr << "Error: --device <deviceId> required" << std::endl;
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
        std::cerr << "Verify failed: could not start the verification transaction" << std::endl;
        pcore::stopSync();
        return 1;
    }
    std::string txnId = txn->transactionId;
    auto& vm = core.sync->verificationManager();

    std::cout << "Verification started with " << targetUser << "/" << targetDevice
              << " (txn " << txnId.substr(0, 8) << "...) — waiting for the other device..."
              << std::endl;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool confirmed = false;
    int rc = 1;

    while (matrixcli::g_interrupted.load() && std::chrono::steady_clock::now() < deadline) {
        auto* t = vm.findTransaction(txnId);
        if (!t) { std::cerr << "Verify failed: transaction gone" << std::endl; break; }

        if (t->state == progressive::desktop::VerificationState::Cancelled) {
            std::cerr << "Verification cancelled by the other side ("
                      << progressive::desktop::cancelCodeToString(
                             t->cancelCode.value_or(progressive::desktop::CancelCode::Other)) << ")" << std::endl;
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
        if (t->state == progressive::desktop::VerificationState::Done) {
            core.store->saveVerifiedDevice(targetUser, targetDevice);
            if (json_out) {
                nlohmann::json j;
                j["ok"] = true;
                j["user_id"] = targetUser;
                j["device_id"] = targetDevice;
                std::cout << j.dump() << std::endl;
            } else {
                std::cout << ANSI_GREEN "\n  ✓ Device verified: " << targetUser << "/" << targetDevice << ANSI_RESET << std::endl;
            }
            rc = 0;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (rc != 0 && !confirmed) {
        if (!matrixcli::g_interrupted.load()) {
            std::cerr << "Verification interrupted (Ctrl+C)" << std::endl;
            controller.cancelVerification(txnId, "m.user");
        } else {
            std::cerr << "Verification timed out after " << timeoutSec << "s" << std::endl;
            controller.cancelVerification(txnId, "m.timeout");
        }
    }
    pcore::stopSync();
    return rc;
}

void registerE2eeCommands() {
    auto& reg = CommandRegistry::instance();
    reg.registerCli("e2ee", cmdE2ee, "E2EE status and key management (status/upload/fallback)");
    reg.registerCli("backup", cmdBackup, "Key backup: create/upload/restore/delete (--recovery-key)");
    reg.registerCli("crosssign", cmdCrossSign, "Cross-signing: setup/reset (--password for UIA)");
    reg.registerCli("ssss", cmdSsss, "Secret storage: upload/retrieve (--recovery-key)");
    reg.registerCli("verify", cmdVerify, "SAS-verify a device: verify <user> --device <id> [--confirm]");
}
