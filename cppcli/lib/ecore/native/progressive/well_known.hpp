#ifndef PROGRESSIVE_WELL_KNOWN_HPP
#define PROGRESSIVE_WELL_KNOWN_HPP

#include <string>

namespace progressive {

// ---- Matrix Well-Known / Server Discovery ----
// Ported from: org.matrix.android.sdk.api.MatrixConfiguration.kt (wellknown discovery)
//              org.matrix.android.sdk.internal.network.WellKnown.kt
//              im.vector.app.features.login.LoginServerUrlFormatter.kt
//
// Original Kotlin flow:
//   1. GET https://domain/.well-known/matrix/client
//   2. Parse JSON → extract "m.homeserver".base_url
//   3. Also extract "m.identity_server".base_url if present
//   4. Validate URLs (must be https://)
//   5. Return ServerDiscoveryResult
//
// This is the parsing/validation logic — HTTP fetching is done by Kotlin.

struct ServerDiscoveryResult {
    std::string homeserverBaseUrl;    // e.g. https://matrix.example.com
    std::string identityServerUrl;    // e.g. https://vector.im
    bool isValid = false;            // at least homeserver present + valid
    std::string error;               // human-readable error message
};

// Parse the .well-known/matrix/client JSON response.
// Original Kotlin (WellKnown.kt:fromJson):
//   fun fromJson(json: JSONObject): WellKnown {
//       val homeServer = json.optJSONObject("m.homeserver")
//       val baseUrl = homeServer?.optString("base_url")
//       return WellKnown(baseUrl, ...)
//   }
ServerDiscoveryResult parseServerDiscovery(const std::string& json);

// Format a user-entered server URL (e.g. "matrix.org" → "https://matrix.org").
// Original Kotlin (LoginServerUrlFormatter.kt:format):
//   fun format(userInput: String): String {
//       var url = userInput.trim()
//       if (!url.startsWith("http")) url = "https://$url"
//       return url.removeSuffix("/")
//   }
std::string formatServerUrl(const std::string& userInput);

// Extract the domain from a user ID or Matrix ID.
// "@user:matrix.org" → "matrix.org"
// "!room:matrix.org" → "matrix.org"
// "#alias:matrix.org" → "matrix.org"
std::string extractServerName(const std::string& mxid);

// Build the well-known URL for a given domain.
// "matrix.org" → "https://matrix.org/.well-known/matrix/client"
// NOTE: This function is implemented in login_utils.hpp/cpp.
// std::string buildWellKnownUrl(const std::string& domain);

// Validate that a homeserver URL looks correct.
// Must be https://, must not contain path segments (just domain:port).
bool isValidHomeserverUrl(const std::string& url);

// Validate an identity server URL.
// Must be https://, path is allowed (e.g. https://vector.im/_matrix/identity).
bool isValidIdentityServerUrl(const std::string& url);

// Format the result as JSON for the Kotlin UI layer.
std::string wellKnownToJson(const ServerDiscoveryResult& result);

// Check if a server URL needs a well-known lookup.
// Returns true if the URL is a plain domain (no scheme, no port, no path).
// "matrix.org" → true
// "https://matrix.org" → false
// "matrix.org:8448" → false
bool needsWellKnownLookup(const std::string& serverInput);

} // namespace progressive

#endif // PROGRESSIVE_WELL_KNOWN_HPP
