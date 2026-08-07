#include <olm/olm.h>
#include <progressive/olm.hpp>
#include "core/crypto/olm_account.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <cstdint>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)
#define CHECK_EQ(a, b, msg) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << msg << " (expected " << (b) << " got " << (a) << ") line " << __LINE__ << "\n"; failures++; } \
    else { std::cout << "ok: " << msg << "\n"; } \
} while (0)

static std::string errorStr(::OlmSession* s) {
    auto e = olm_session_last_error(s);
    return e ? e : "(no error)";
}

static std::string errorStr(::OlmAccount* a) {
    auto e = olm_account_last_error(a);
    return e ? e : "(no error)";
}

static int my_rand() {
    static int x = 12345;
    x = x * 1103515245 + 12345;
    return (x >> 16) & 0x7fff;
}
static void fill_random(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)(my_rand() & 0xff);
}

static std::string extractFirstOTK(const std::string& json) {
    auto pos = json.find("\"AAAAAA\":\"");
    if (pos == std::string::npos) {
        pos = json.find("\"curve25519\":{");
        if (pos == std::string::npos) return "";
        pos = json.find("\":\"", pos);
        if (pos == std::string::npos) return "";
        pos += 3;
    } else {
        pos += 10;
    }
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static void test_olm_roundtrip() {
    // Bob: create account
    std::vector<uint8_t> bob_acct_buf(::olm_account_size());
    ::OlmAccount* bobAcc = ::olm_account(bob_acct_buf.data());
    size_t rnd = ::olm_create_account_random_length(bobAcc);
    std::vector<uint8_t> rndBuf(rnd);
    fill_random(rndBuf.data(), rnd);
    int rc = ::olm_create_account(bobAcc, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "Bob account created");
    if (rc == ::olm_error()) std::cerr << "  err=" << errorStr(bobAcc) << "\n";

    // Bob: generate OTKs
    rnd = ::olm_account_generate_one_time_keys_random_length(bobAcc, 5);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_account_generate_one_time_keys(bobAcc, 5, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "Bob generated OTKs");
    if (rc == ::olm_error()) std::cerr << "  err=" << errorStr(bobAcc) << "\n";

    // Get Bob's identity keys JSON
    size_t idLen = ::olm_account_identity_keys_length(bobAcc);
    std::vector<uint8_t> bobIdKeys(idLen);
    ::olm_account_identity_keys(bobAcc, bobIdKeys.data(), idLen);
    std::cout << "  Bob identity: " << std::string((char*)bobIdKeys.data(), idLen) << "\n";

    // Get Bob's OTK JSON
    size_t otkLen = ::olm_account_one_time_keys_length(bobAcc);
    std::vector<uint8_t> bobOtKeys(otkLen);
    ::olm_account_one_time_keys(bobAcc, bobOtKeys.data(), otkLen);
    std::cout << "  Bob OTKs: " << std::string((char*)bobOtKeys.data(), otkLen) << "\n";

    // Alice: create account
    std::vector<uint8_t> alice_acct_buf(::olm_account_size());
    ::OlmAccount* aliceAcc = ::olm_account(alice_acct_buf.data());
    rnd = ::olm_create_account_random_length(aliceAcc);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_account(aliceAcc, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "Alice account created");

    // Alice: get identity keys for verification
    idLen = ::olm_account_identity_keys_length(aliceAcc);
    std::vector<uint8_t> aliceIdKeys(idLen);
    ::olm_account_identity_keys(aliceAcc, aliceIdKeys.data(), idLen);

    // Alice: create outbound session using Bob's base64 keys
    // b_id_keys.data() + 15 = curve25519 identity key base64 (43 chars)
    // b_ot_keys.data() + 25 = first OTK base64 (43 chars)
    // The "+15" skips: {"curve25519":"  (15 chars)
    // The "+25" skips: {"curve25519":{"AAAAAA":","AAAAAA":"  (25 chars... wait)

    // Bob's identity JSON: {"curve25519":"<b64>","ed25519":"<b64>"}
    // Bob's OTK JSON (nested): {"curve25519":{"AAAAqg":"<b64>","AAAAqQ":"<b64>"}}

    // For identity keys: +15 = skip {"curve25519":" → points to curve25519 b64
    // For OTK keys (nested): we need to find the first b64 value
    // {"curve25519":{" → 14 chars, then "keyID":" → varies, then b64 value

    // Extract Bob's curve25519 identity key base64 from JSON
    auto idStr = std::string((char*)bobIdKeys.data(), idLen);
    auto pos = idStr.find("\"curve25519\":\"");
    CHECK(pos != std::string::npos, "Find curve25519 in identity keys");
    auto ikStart = pos + 14;  // skip "curve25519":"
    auto ikEnd = idStr.find('"', ikStart);
    std::string bobIkB64 = idStr.substr(ikStart, ikEnd - ikStart);
    CHECK(bobIkB64.size() == 43, "Bob IK base64 is 43 chars");

    // Extract Bob's first OTK base64 from JSON (nested format)
    auto otkStr = std::string((char*)bobOtKeys.data(), otkLen);
    pos = otkStr.find("\"curve25519\":{");
    CHECK(pos != std::string::npos, "Find curve25519 obj in OTK JSON");
    pos = otkStr.find("\":\"", pos);  // skip to first key's value
    CHECK(pos != std::string::npos, "Find first OTK value");
    auto otkStart = pos + 3;  // skip ":"
    auto otkEnd = otkStr.find('"', otkStart);
    std::string bobOtkB64 = otkStr.substr(otkStart, otkEnd - otkStart);
    CHECK(bobOtkB64.size() == 43, "Bob OTK base64 is 43 chars");
    std::cout << "  Bob IK: " << bobIkB64.substr(0,8) << "...\n";
    std::cout << "  Bob OTK: " << bobOtkB64.substr(0,8) << "...\n";

    // Alice: create outbound session
    std::vector<uint8_t> a_sess_buf(::olm_session_size());
    ::OlmSession* aSess = ::olm_session(a_sess_buf.data());
    rnd = ::olm_create_outbound_session_random_length(aSess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_outbound_session(aSess, aliceAcc,
        bobIkB64.data(), 43,
        bobOtkB64.data(), 43,
        rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "Alice createOutbound");
    if (rc == ::olm_error()) std::cerr << "  err=" << errorStr(aSess) << "\n";

    // Alice: encrypt
    std::string plaintext = "Hello from Alice!";
    rnd = ::olm_encrypt_random_length(aSess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    size_t msgLen = ::olm_encrypt_message_length(aSess, plaintext.size());
    std::vector<uint8_t> msg(msgLen);
    size_t written = ::olm_encrypt(aSess, (void*)plaintext.data(), plaintext.size(),
        rndBuf.data(), rnd, msg.data(), msgLen);
    CHECK(written != ::olm_error(), "Alice encrypt");
    int msgType = ::olm_encrypt_message_type(aSess);
    CHECK_EQ(msgType, 0, "Pre-key message type is 0");
    std::string encMsg((char*)msg.data(), written);

    // Bob: create inbound session from pre-key
    std::vector<uint8_t> b_sess_buf(::olm_session_size());
    ::OlmSession* bSess = ::olm_session(b_sess_buf.data());
    std::vector<uint8_t> tmpMsg(msg);
    rc = ::olm_create_inbound_session(bSess, bobAcc, tmpMsg.data(), written);
    CHECK(rc != ::olm_error(), "Bob createInbound");
    if (rc == ::olm_error()) std::cerr << "  err=" << errorStr(bSess) << "\n";

    // Bob: decrypt — restore message copy
    tmpMsg.resize(written);
    std::memcpy(tmpMsg.data(), msg.data(), written);
    size_t ptLen = ::olm_decrypt_max_plaintext_length(bSess, 0, tmpMsg.data(), written);
    if (ptLen == ::olm_error()) std::cerr << "  decrypt ptLen error: " << errorStr(bSess) << "\n";
    CHECK(ptLen != ::olm_error(), "Bob decrypt ptLen valid");
    std::vector<uint8_t> pt(ptLen);
    tmpMsg.resize(written);
    std::memcpy(tmpMsg.data(), msg.data(), written);
    size_t ptWritten = ::olm_decrypt(bSess, 0, tmpMsg.data(), written, pt.data(), ptLen);
    if (ptWritten == ::olm_error()) std::cerr << "  decrypt error: " << errorStr(bSess) << "\n";
    CHECK(ptWritten != ::olm_error(), "Bob decrypt");
    std::string decrypted((char*)pt.data(), ptWritten);
    CHECK_EQ(decrypted, plaintext, "Plaintext matches!");

    ::olm_clear_session(aSess);
    ::olm_clear_session(bSess);
    ::olm_clear_account(bobAcc);
    ::olm_clear_account(aliceAcc);

    std::cout << "--- test_olm_roundtrip PASSED ---\n";
}

static void test_real_account_roundtrip(const std::string& pickleRaw, const std::string& pickleKey) {
    std::cout << "\n--- test_real_account_roundtrip ---\n";

    // Step 1: Load Bob's real account from session.db
    std::cout << "  pickle raw size=" << pickleRaw.size() << " key=" << pickleKey << "\n";

    progressive::OlmAccount bob;
    auto up = bob.unpickle(pickleKey, pickleRaw);
    CHECK(up.success, "Unpickle Bob's real account");

    std::string bobCurveB64 = bob.curve25519Key().data;
    CHECK(!bobCurveB64.empty(), "Got Bob's real curve25519");
    std::cout << "  Bob real curve25519: " << bobCurveB64.substr(0, 8) << "...\n";

    // Step 2: Generate FRESH one-time keys for Bob's real account
    auto otk = bob.generateOneTimeKeys(5);
    CHECK(otk.success, "Bob real account generated fresh OTKs");
    std::string otkJson = otk.data;
    CHECK(!otkJson.empty(), "Fresh OTK JSON not empty");

    std::string b64Key = extractFirstOTK(otkJson);
    CHECK(!b64Key.empty(), "Extracted first fresh OTK");
    std::cout << "  Bob fresh OTK: " << b64Key.substr(0, 8) << "...\n";

    // Step 3: Alice creates outbound session using Bob's fresh OTK
    progressive::OlmAccount aliceAccount;
    auto a = aliceAccount.create();
    CHECK(a.success, "Alice account created");

    progressive::OlmSession aliceSession;
    auto out = aliceSession.createOutbound(aliceAccount, bobCurveB64, b64Key);
    CHECK(out.success, "Alice createOutbound with Bob's real account + fresh OTK");

    std::string original = "Hello from Alice to the real Bob!";
    auto enc = aliceSession.encrypt(original);
    CHECK(enc.success, "Alice encrypt");
    CHECK_EQ(enc.messageType, 0, "Pre-key message type is 0");

    // Step 4: Bob's real account receives and decrypts
    // createInbound mutates the pre-key buffer — copy before, restore before decrypt
    std::string encCopy = enc.data;
    progressive::OlmSession bobSession;
    auto in = bobSession.createInbound(bob, encCopy);
    CHECK(in.success, "Bob real account createInbound");

    encCopy = enc.data;  // restore original
    auto dec = bobSession.decrypt(encCopy, 0);
    CHECK(dec.success, "Bob real account decrypt");

    CHECK_EQ(dec.data, original, "Plaintext matches with real account!");

    std::cout << "--- test_real_account_roundtrip PASSED ---\n";
}

// Bob generates a fallback key; Alice "claims" it and creates an outbound
// session just like with an OTK. Validates libolm lookup_key fallback branch
// (account.cpp:35-49 current_fallback_key + prev_fallback_key).
static void test_olm_fallback_key() {
    // Bob: create account
    std::vector<uint8_t> bobBuf(::olm_account_size());
    ::OlmAccount* bobAcc = ::olm_account(bobBuf.data());
    size_t rnd = ::olm_create_account_random_length(bobAcc);
    std::vector<uint8_t> rndBuf(rnd);
    fill_random(rndBuf.data(), rnd);
    int rc = ::olm_create_account(bobAcc, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Bob account created");

    // Bob: generate fallback key
    rnd = ::olm_account_generate_fallback_key_random_length(bobAcc);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_account_generate_fallback_key(bobAcc, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Bob generated fallback key");

    // Bob: get fallback key JSON
    size_t fkLen = ::olm_account_unpublished_fallback_key_length(bobAcc);
    CHECK(fkLen > 0, "fb: fallback key length > 0");
    std::vector<uint8_t> fkJson(fkLen);
    ::olm_account_unpublished_fallback_key(bobAcc, fkJson.data(), fkLen);
    std::string fkStr((char*)fkJson.data(), fkLen);

    // Bob: get identity keys
    size_t idLen = ::olm_account_identity_keys_length(bobAcc);
    std::vector<uint8_t> bobIdKeys(idLen);
    ::olm_account_identity_keys(bobAcc, bobIdKeys.data(), idLen);
    std::string idStr((char*)bobIdKeys.data(), idLen);

    // Extract Bob's curve25519 identity key base64
    auto pos = idStr.find("\"curve25519\":\"");
    CHECK(pos != std::string::npos, "fb: find curve25519 in identity keys");
    auto ikStart = pos + 14;
    auto ikEnd = idStr.find('"', ikStart);
    std::string bobIkB64 = idStr.substr(ikStart, ikEnd - ikStart);
    CHECK(bobIkB64.size() == 43, "fb: Bob IK base64 is 43 chars");

    // Extract fallback key value base64 from nested JSON
    pos = fkStr.find("\"curve25519\":{\"");
    CHECK(pos != std::string::npos, "fb: find curve25519 object in fallback JSON");
    pos = fkStr.find("\":\"", pos);
    CHECK(pos != std::string::npos, "fb: find key value in fallback JSON");
    auto fkStart = pos + 3;
    auto fkEnd = fkStr.find('"', fkStart);
    std::string bobFkB64 = fkStr.substr(fkStart, fkEnd - fkStart);
    CHECK(bobFkB64.size() == 43, "fb: fallback key base64 is 43 chars");

    // Alice: create account
    std::vector<uint8_t> aliceBuf(::olm_account_size());
    ::OlmAccount* aliceAcc = ::olm_account(aliceBuf.data());
    rnd = ::olm_create_account_random_length(aliceAcc);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_account(aliceAcc, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Alice account created");

    // Alice: create outbound session using Bob's IK + fallback key
    std::vector<uint8_t> sessBuf(::olm_session_size());
    ::OlmSession* aSess = ::olm_session(sessBuf.data());
    rnd = ::olm_create_outbound_session_random_length(aSess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_outbound_session(aSess, aliceAcc,
        bobIkB64.data(), 43,
        bobFkB64.data(), 43,
        rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Alice created outbound session with fallback key");

    // Alice: encrypt
    std::string fbText = "fallback";
    rnd = ::olm_encrypt_random_length(aSess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    size_t msgLen = ::olm_encrypt_message_length(aSess, fbText.size());
    std::vector<uint8_t> msg(msgLen);
    size_t written = ::olm_encrypt(aSess, (void*)fbText.data(), fbText.size(),
        rndBuf.data(), rnd, msg.data(), msgLen);
    CHECK(written != ::olm_error(), "fb: Alice encrypted message");

    // Bob: create inbound session from Alice's initial message
    std::vector<uint8_t> bSessBuf(::olm_session_size());
    ::OlmSession* bSess = ::olm_session(bSessBuf.data());
    std::vector<uint8_t> tmpMsg(msg.begin(), msg.begin() + written);
    rc = ::olm_create_inbound_session(bSess, bobAcc, tmpMsg.data(), written);
    CHECK(rc != ::olm_error(), "fb: Bob created inbound session (lookup_key matched fallback)");

    // Bob: decrypt — restore message copy
    tmpMsg.resize(written);
    std::memcpy(tmpMsg.data(), msg.data(), written);
    size_t ptLen = ::olm_decrypt_max_plaintext_length(bSess, 0, tmpMsg.data(), written);
    CHECK(ptLen != ::olm_error(), "fb: Bob decrypt ptLen valid");
    std::vector<uint8_t> ptBuf(ptLen);
    tmpMsg.resize(written);
    std::memcpy(tmpMsg.data(), msg.data(), written);
    int dc = ::olm_decrypt(bSess, 0, tmpMsg.data(), written, ptBuf.data(), ptLen);
    CHECK(dc != ::olm_error(), "fb: Bob decrypted Alice's message");
    std::string fbResult((char*)ptBuf.data(), dc);
    CHECK(fbResult == "fallback", "fb: decrypted text matches");

    // --- prev_fallback_key branch + forgetOldFallbackKey ---

    // Remember key#1 for late pre-key use after generation #2.
    std::string key1B64 = bobFkB64;
    ::OlmAccount* bobAcc2 = bobAcc;  // same account

    // Bob: generate fallback #2 — rotates #1 to prev_fallback_key.
    rnd = ::olm_account_generate_fallback_key_random_length(bobAcc2);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_account_generate_fallback_key(bobAcc2, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Bob generated fallback #2 (key#1 -> prev)");

    // Alice: create outbound session using key#1 (simulates late pre-key to old key).
    std::vector<uint8_t> a2SessBuf(::olm_session_size());
    ::OlmSession* a2Sess = ::olm_session(a2SessBuf.data());
    rnd = ::olm_create_outbound_session_random_length(a2Sess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_outbound_session(a2Sess, aliceAcc,
        bobIkB64.data(), 43, key1B64.data(), 43, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Alice outbound from key#1 (prev branch)");

    // Alice encrypt.
    std::string pvText = "prevkey";
    rnd = ::olm_encrypt_random_length(a2Sess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    size_t pvMsgLen = ::olm_encrypt_message_length(a2Sess, pvText.size());
    std::vector<uint8_t> pvMsg(pvMsgLen);
    size_t pvWritten = ::olm_encrypt(a2Sess, (void*)pvText.data(), pvText.size(),
        rndBuf.data(), rnd, pvMsg.data(), pvMsgLen);
    CHECK(pvWritten != ::olm_error(), "fb: Alice encrypted prev-key message");

    // Bob: create inbound — must match prev_fallback_key (account.cpp:43-48).
    std::vector<uint8_t> b2SessBuf(::olm_session_size());
    ::OlmSession* b2Sess = ::olm_session(b2SessBuf.data());
    std::vector<uint8_t> pvTmp(pvMsg.begin(), pvMsg.begin() + pvWritten);
    rc = ::olm_create_inbound_session(b2Sess, bobAcc2, pvTmp.data(), pvWritten);
    CHECK(rc != ::olm_error(), "fb: Bob inbound from key#1 (prev_fallback_key branch)");

    // Decrypt prev-key message.
    pvTmp.resize(pvWritten);
    std::memcpy(pvTmp.data(), pvMsg.data(), pvWritten);
    size_t pvPtLen = ::olm_decrypt_max_plaintext_length(b2Sess, 0, pvTmp.data(), pvWritten);
    CHECK(pvPtLen != ::olm_error(), "fb: prev-key decrypt ptLen valid");
    std::vector<uint8_t> pvPt(pvPtLen);
    pvTmp.resize(pvWritten);
    std::memcpy(pvTmp.data(), pvMsg.data(), pvWritten);
    int pvDc = ::olm_decrypt(b2Sess, 0, pvTmp.data(), pvWritten, pvPt.data(), pvPtLen);
    CHECK(pvDc != ::olm_error(), "fb: prev-key decrypted");
    std::string pvResult((char*)pvPt.data(), pvDc);
    CHECK(pvResult == "prevkey", "fb: prev-key text matches");

    // Forget old fallback key — key#1 should be gone.
    ::olm_account_forget_old_fallback_key(bobAcc2);

    // Alice: new outbound from key#1 — should fail (key gone).
    std::vector<uint8_t> a3SessBuf(::olm_session_size());
    ::OlmSession* a3Sess = ::olm_session(a3SessBuf.data());
    rnd = ::olm_create_outbound_session_random_length(a3Sess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    rc = ::olm_create_outbound_session(a3Sess, aliceAcc,
        bobIkB64.data(), 43, key1B64.data(), 43, rndBuf.data(), rnd);
    CHECK(rc != ::olm_error(), "fb: Alice outbound from key#1 after forget");

    std::string fgText = "forgotten";
    rnd = ::olm_encrypt_random_length(a3Sess);
    rndBuf.resize(rnd);
    fill_random(rndBuf.data(), rnd);
    size_t fgMsgLen = ::olm_encrypt_message_length(a3Sess, fgText.size());
    std::vector<uint8_t> fgMsg(fgMsgLen);
    size_t fgWritten = ::olm_encrypt(a3Sess, (void*)fgText.data(), fgText.size(),
        rndBuf.data(), rnd, fgMsg.data(), fgMsgLen);
    CHECK(fgWritten != ::olm_error(), "fb: Alice encrypted post-forget message");

    // Bob: inbound should FAIL — key#1 was forgotten.
    std::vector<uint8_t> b3SessBuf(::olm_session_size());
    ::OlmSession* b3Sess = ::olm_session(b3SessBuf.data());
    std::vector<uint8_t> fgTmp(fgMsg.begin(), fgMsg.begin() + fgWritten);
    rc = ::olm_create_inbound_session(b3Sess, bobAcc2, fgTmp.data(), fgWritten);
    CHECK(rc == ::olm_error(), "fb: Bob inbound FAILS after forget (key#1 gone)");

}


int main(int argc, char** argv) {
    std::cout << "=== Olm Inbound Test (raw C API) ===\n\n";

    // Always run the synthetic roundtrip
    test_olm_roundtrip();
    test_olm_fallback_key();

    // If pickle + key provided, test real account roundtrip
    if (argc >= 3) {
        std::string pickleRaw = argv[1];
        std::string pickleKey = argv[2];
        test_real_account_roundtrip(pickleRaw, pickleKey);
    } else {
        std::cout << "\nUsage for real account test:\n";
        std::cout << "  " << argv[0] << " <pickle_raw> <pickle_key>\n";
    }

    if (failures == 0) { std::cout << "\nALL TESTS PASSED\n"; return 0; }
    std::cerr << failures << " FAILURE(S)\n"; return 1;
}
