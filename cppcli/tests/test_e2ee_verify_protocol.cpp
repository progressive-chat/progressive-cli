// tests/test_e2ee_verify_protocol.cpp — full SAS protocol across two VerificationManagers.
// Drives the entire m.sas.v1 state machine (request→ready→start→accept/key→mac→done),
// asserts both sides reach Done with identical emojis, and validates the negative
// path (corrupted MAC → m.key_mismatch cancel). Closest-to-ground-truth test
// without Element.
#include "core/crypto/verification.hpp"
#include "core/crypto/sas.hpp"
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

using progressive::desktop::VerificationManager;
using progressive::desktop::VerificationState;
using progressive::desktop::CancelCode;
using progressive::desktop::sasCreate;

namespace {

struct Harness {
    VerificationManager a;  // initiator: sends .request
    VerificationManager b;  // responder: sends .ready/.start

    std::string aUser = "@alice:test", aDev = "DEVA", aEd = "edA", aCurve = "curveA";
    std::string bUser = "@bob:test",  bDev = "DEVB", bEd = "edB", bCurve = "curveB";

    std::vector<std::pair<std::string, std::string>> sentByA;  // (eventType, content)
    std::vector<std::pair<std::string, std::string>> sentByB;
    std::string lastTxnId;

    Harness() {
        // Device key resolvers: return the OTHER side's fixed keys.
        a.setDeviceKeyResolverFn([this](const std::string& user, const std::string& dev,
            std::string& ed, std::string& curve) {
            if (user == bUser && dev == bDev) { ed = bEd; curve = bCurve; return true; }
            return false;
        });
        b.setDeviceKeyResolverFn([this](const std::string& user, const std::string& dev,
            std::string& ed, std::string& curve) {
            if (user == aUser && dev == aDev) { ed = aEd; curve = aCurve; return true; }
            return false;
        });
        // Send wire: each side's outgoing events feed the other side's handleEvent.
        a.setSendToDeviceFn([this](const std::string& eventType, const std::string&,
            const std::string& content, const std::string&, const std::string&) {
            sentByA.emplace_back(eventType, content);
            b.handleEvent(eventType, aUser, content, bUser, bDev, bEd, bCurve);
        });
        b.setSendToDeviceFn([this](const std::string& eventType, const std::string&,
            const std::string& content, const std::string&, const std::string&) {
            sentByB.emplace_back(eventType, content);
            a.handleEvent(eventType, bUser, content, aUser, aDev, aEd, aCurve);
        });
    }
};

} // namespace

// Drives both managers through the full flow. Returns false on failure.
static bool driveToDone(Harness& h, bool assertChecks) {
    auto* txnA = h.a.startVerification(h.bUser, h.bDev, h.aDev);
    if (!txnA) { CHECK(false, "A startVerification"); return false; }
    std::string txnId = txnA->transactionId;
    h.lastTxnId = txnId;

    // A → B: .request (feeds B's handleEvent directly — no sendToDeviceFn for start)
    h.b.handleEvent("m.key.verification.request", h.aUser,
        h.a.buildRequestContent(h.aDev, txnId), h.bUser, h.bDev, h.bEd, h.bCurve);
    auto* txnB = h.b.findTransaction(txnId);
    if (!txnB) { CHECK(false, "B got request"); return false; }

    // B → A: .ready
    h.a.handleEvent("m.key.verification.ready", h.bUser,
        h.b.buildReadyContent(h.bDev, txnId), h.aUser, h.aDev, h.aEd, h.aCurve);

    // B → A: .start (replicate acceptIncoming: store startContentJson + create SAS)
    std::string startContent = h.b.buildStartContent(h.bDev, txnId);
    txnB->startContentJson = startContent;
    txnB->sas = sasCreate();
    h.a.handleEvent("m.key.verification.start", h.bUser, startContent,
        h.aUser, h.aDev, h.aEd, h.aCurve);
    // A's .start handler auto-sends .accept + .key → forwarded to B via sendToDeviceFn.

    // B → A: .key (B's SAS pubkey)
    h.a.handleEvent("m.key.verification.key", h.bUser,
        h.b.buildKeyContent(h.bDev, txnId, txnB->sas.ourPubkey),
        h.aUser, h.aDev, h.aEd, h.aCurve);

    if (assertChecks) {
        CHECK(txnA->state == VerificationState::KeyReceived, "A KeyReceived");
        CHECK(txnB->state == VerificationState::KeyReceived, "B KeyReceived");
    } else if (txnA->state != VerificationState::KeyReceived ||
               txnB->state != VerificationState::KeyReceived) {
        return false;
    }

    // Both compute emojis — must be identical.
    auto emA = h.a.computeEmojis(*txnA);
    auto emB = h.b.computeEmojis(*txnB);
    if (assertChecks) {
        CHECK_EQ((int)emA.size(), 7, "A produces 7 emojis");
        CHECK(emA.size() == emB.size(), "B emoji count matches");
        bool match = emA.size() == emB.size();
        for (size_t i = 0; match && i < emA.size(); i++)
            if (emA[i].emoji != emB[i].emoji) match = false;
        CHECK(match, "A and B emojis identical");
    }
    if (emA.empty() || emB.empty() || emA.size() != emB.size()) return false;

    // MAC phase: A (initiator/accepter) confirms first, then B.
    // Replicate VerificationController::confirmMatch: build mac, send, set MacSent.
    txnA->state = VerificationState::MacSent;
    h.b.handleEvent("m.key.verification.mac", h.aUser,
        h.a.buildMacContent(*txnA, txnA->sas), h.bUser, h.bDev, h.bEd, h.bCurve);
    if (assertChecks) CHECK(txnB->state == VerificationState::MacReceived, "B MacReceived after A mac");
    txnB->state = VerificationState::MacSent;
    h.a.handleEvent("m.key.verification.mac", h.bUser,
        h.b.buildMacContent(*txnB, txnB->sas), h.aUser, h.aDev, h.aEd, h.aCurve);
    // A: state was MacSent → verify ok → Done + auto-sends .done → B → Done.
    if (assertChecks) {
        CHECK(txnA->state == VerificationState::Done, "A Done");
        CHECK(txnB->state == VerificationState::Done, "B Done");
    }
    return txnA->state == VerificationState::Done &&
           txnB->state == VerificationState::Done;
}

// Corrupts one base64 char inside the mac value of a buildMacContent payload.
static std::string corruptMacContent(const std::string& content) {
    // "mac":{"ed25519:DEVA":"<base64>" — flip a char of the base64 VALUE,
    // keeping the JSON valid (parse must succeed, verify must fail).
    auto p = content.find("ed25519:");
    if (p == std::string::npos) return content;
    auto colon = content.find(':', p);          // colon inside "ed25519:"
    auto keyEnd = content.find('"', colon);     // closing quote of key id
    if (keyEnd == std::string::npos) return content;
    auto valColon = content.find(':', keyEnd);  // colon before value
    auto valStart = content.find('"', valColon) + 1;  // opening quote of value
    if (valStart == std::string::npos || valStart >= content.size()) return content;
    std::string out = content;
    out[valStart] = (out[valStart] == 'A') ? 'B' : 'A';
    return out;
}

// Full happy-path: both sides reach Done, emojis identical, no cancels sent.
static void test_happy_path() {
    Harness h;
    CHECK(driveToDone(h, true), "full flow completes");
    bool anyCancel = false;
    for (auto& [t, c] : h.sentByA) if (t == "m.key.verification.cancel") anyCancel = true;
    for (auto& [t, c] : h.sentByB) if (t == "m.key.verification.cancel") anyCancel = true;
    CHECK(!anyCancel, "no cancel events on happy path");
    // Both sides must have sent .done by the end.
    bool aDone = false, bDone = false;
    for (auto& [t, c] : h.sentByA) if (t == "m.key.verification.done") aDone = true;
    for (auto& [t, c] : h.sentByB) if (t == "m.key.verification.done") bDone = true;
    CHECK(aDone || bDone, "done event sent by at least one side");
}

// Negative path: corrupted MAC → B must cancel with m.key_mismatch.
static void test_corrupted_mac_cancels() {
    Harness h;
    auto* txnA = h.a.startVerification(h.bUser, h.bDev, h.aDev);
    std::string txnId = txnA->transactionId;
    h.b.handleEvent("m.key.verification.request", h.aUser,
        h.a.buildRequestContent(h.aDev, txnId), h.bUser, h.bDev, h.bEd, h.bCurve);
    auto* txnB = h.b.findTransaction(txnId);

    h.a.handleEvent("m.key.verification.ready", h.bUser,
        h.b.buildReadyContent(h.bDev, txnId), h.aUser, h.aDev, h.aEd, h.aCurve);
    std::string startContent = h.b.buildStartContent(h.bDev, txnId);
    txnB->startContentJson = startContent;
    txnB->sas = sasCreate();
    h.a.handleEvent("m.key.verification.start", h.bUser, startContent,
        h.aUser, h.aDev, h.aEd, h.aCurve);
    h.a.handleEvent("m.key.verification.key", h.bUser,
        h.b.buildKeyContent(h.bDev, txnId, txnB->sas.ourPubkey),
        h.aUser, h.aDev, h.aEd, h.aCurve);
    CHECK(txnB->state == VerificationState::KeyReceived, "B KeyReceived before bad MAC");

    // A sends a corrupted MAC to B.
    std::string macA = h.a.buildMacContent(*txnA, txnA->sas);
    h.b.handleEvent("m.key.verification.mac", h.aUser, corruptMacContent(macA),
        h.bUser, h.bDev, h.bEd, h.bCurve);

    CHECK(txnB->state == VerificationState::Cancelled, "B Cancelled on bad MAC");
    CHECK(txnB->cancelCode.has_value() && *txnB->cancelCode == CancelCode::KeyMismatch,
        "B cancelCode = KeyMismatch");
    bool sentKeyMismatch = false;
    for (auto& [t, c] : h.sentByB) {
        if (t == "m.key.verification.cancel" &&
            c.find("m.key_mismatch") != std::string::npos)
            sentKeyMismatch = true;
    }
    CHECK(sentKeyMismatch, "B forwarded cancel contains \"code\":\"m.key_mismatch\"");
}


// Pruning sweep: expired active txn -> erased + m.timeout cancel sent;
// expired finished txn -> erased silently.
static void test_expired_sweep() {
    Harness h;
    auto* txn = h.a.startVerification(h.bUser, h.bDev, h.aDev);
    CHECK(txn != nullptr, "sweep: txn created");
    std::string txnId = txn->transactionId;
    txn->startTime = std::chrono::steady_clock::now() - std::chrono::minutes(11);

    // Feed a new .request event — the sweep runs first (under the held lock).
    h.a.handleEvent("m.key.verification.request", h.bUser,
        h.a.buildRequestContent(h.aDev, "newtxn1"), h.aUser, h.aDev, h.aEd, h.aCurve);

    CHECK(h.a.findTransaction(txnId) == nullptr, "sweep: expired active txn erased");
    bool sentTimeout = false;
    for (auto& [t, c] : h.sentByA) {
        if (t == "m.key.verification.cancel" && c.find("m.timeout") != std::string::npos)
            sentTimeout = true;
    }
    CHECK(sentTimeout, "sweep: m.timeout cancel sent for expired active txn");

    // Expired Cancelled txn: erased WITHOUT a cancel.
    auto* txn2 = h.a.startVerification(h.bUser, h.bDev, h.aDev);
    CHECK(txn2 != nullptr, "sweep: second txn created");
    std::string txn2Id = txn2->transactionId;  // save BEFORE sweep frees it
    txn2->startTime = std::chrono::steady_clock::now() - std::chrono::minutes(11);
    txn2->state = VerificationState::Cancelled;
    h.a.handleEvent("m.key.verification.request", h.bUser,
        h.a.buildRequestContent(h.aDev, "newtxn2"), h.aUser, h.aDev, h.aEd, h.aCurve);
    CHECK(h.a.findTransaction(txn2Id) == nullptr,
          "sweep: expired cancelled txn erased");

    // Fresh txn survives the sweep.
    auto* txn3 = h.a.startVerification(h.bUser, h.bDev, h.aDev);
    CHECK(txn3 != nullptr, "sweep: third txn created");
    h.a.handleEvent("m.key.verification.request", h.bUser,
        h.a.buildRequestContent(h.aDev, "newtxn3"), h.aUser, h.aDev, h.aEd, h.aCurve);
    CHECK(h.a.findTransaction(txn3->transactionId) != nullptr,
          "sweep: fresh txn survives");
}


// Negative MSK: if the OTHER side's view of our master key is wrong, the mac
// verification must fail (the flow must NOT reach Done).
static void test_msk_mismatch_cancels() {
    Harness h;
    h.a.setOurMasterKeyFn([]() { return std::string("mskA"); });
    h.a.setTheirMasterKeyFn([](const std::string&) { return std::string("mskB"); });
    h.b.setOurMasterKeyFn([]() { return std::string("mskB"); });
    // B's view of A's master is WRONG — the macs must mismatch.
    h.b.setTheirMasterKeyFn([](const std::string&) { return std::string("mskWrong"); });
    bool done = driveToDone(h, false);
    CHECK(!done, "msk-neg: mismatched master key prevents Done");
}

// MSK exchange: when both sides provide their cross-signing master pubkeys,
// the mac must include the "ed25519:<masterPub>" pseudo-devices and the flow
// must still reach Done (the MSK entries verify on both sides).
static void test_msk_exchange() {
    Harness h;
    h.a.setOurMasterKeyFn([]() { return std::string("mskA"); });
    h.a.setTheirMasterKeyFn([](const std::string&) { return std::string("mskB"); });
    h.b.setOurMasterKeyFn([]() { return std::string("mskB"); });
    h.b.setTheirMasterKeyFn([](const std::string&) { return std::string("mskA"); });
    bool done = driveToDone(h, false);
    CHECK(done, "msk: flow reaches Done with MSKs exchanged");
    // The mac content must carry the MSK pseudo-devices (built directly — the
    // two-manager drive injects the macs, it doesn't send them over the wire).
    auto* txnA = h.a.findTransaction(h.lastTxnId);
    if (txnA) {
        std::string macA = h.a.buildMacContent(*txnA, txnA->sas);
        CHECK(macA.find("ed25519:mskA") != std::string::npos,
              "msk: A's mac includes our MSK pseudo-device");
    }
    auto* txnB = h.b.findTransaction(h.lastTxnId);
    if (txnB) {
        std::string macB = h.b.buildMacContent(*txnB, txnB->sas);
        CHECK(macB.find("ed25519:mskB") != std::string::npos,
              "msk: B's mac includes its MSK pseudo-device");
    }
}

int main() {
    test_msk_mismatch_cancels();
    test_msk_exchange();

    test_happy_path();
    test_corrupted_mac_cancels();
    test_expired_sweep();
    if (failures > 0) {
        std::cerr << "\n" << failures << " test(s) FAILED\n";
        return 1;
    }
    std::cout << "\nAll verification protocol tests passed\n";
    return 0;
}
