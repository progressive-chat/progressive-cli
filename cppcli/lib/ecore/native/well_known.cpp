#include "progressive/well_known.hpp"
#include <sstream>
#include <cctype>

namespace progressive {

// ---- JSON parser for well-known response ----
// Original Kotlin (WellKnown.kt):
//   data class WellKnown(
//       @Json(name = "m.homeserver") val homeServer: HomeServer?,
//       @Json(name = "m.identity_server") val identityServer: IdentityServer?
//   )
//   data class HomeServer(@Json(name = "base_url") val baseUrl: String)
//   data class IdentityServer(@Json(name = "base_url") val baseUrl: String)

ServerDiscoveryResult parseServerDiscovery(const std::string& json) {
    ServerDiscoveryResult result;

    // Original Kotlin: json.optJSONObject("m.homeserver")?.optString("base_url")
    auto extractBaseUrl = [&](const std::string& section) -> std::string {
        // Find the section e.g. "m.homeserver"
        auto sectPos = json.find("\"" + section + "\"");
        if (sectPos == std::string::npos) return "";

        // Find the opening brace of the object
        auto openBrace = json.find('{', sectPos);
        if (openBrace == std::string::npos) return "";

        // Find "base_url" within this object
        auto baseUrlPos = json.find("\"base_url\"", openBrace);
        if (baseUrlPos == std::string::npos) return "";

        // Find the closing brace to bound the search
        auto closeBrace = json.find('}', openBrace);
        if (closeBrace == std::string::npos) return "";

        // base_url must be inside this object
        if (baseUrlPos > closeBrace) return "";

        // Extract the string value after "base_url":
        auto colon = json.find(':', baseUrlPos);
        if (colon == std::string::npos) return "";

        // Skip to first quote
        auto quoteStart = json.find('"', colon);
        if (quoteStart == std::string::npos) return "";

        auto quoteEnd = json.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos) return "";

        return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    };

    result.homeserverBaseUrl = extractBaseUrl("m.homeserver");
    result.identityServerUrl = extractBaseUrl("m.identity_server");

    // Validate
    result.isValid = isValidHomeserverUrl(result.homeserverBaseUrl);

    if (!result.isValid) {
        if (result.homeserverBaseUrl.empty()) {
            result.error = "Missing m.homeserver.base_url in well-known response";
        } else {
            result.error = "Invalid homeserver URL: must be https:// (got: " +
                          result.homeserverBaseUrl + ")";
        }
    }

    return result;
}

// ---- URL formatting ----
// Original Kotlin (LoginServerUrlFormatter.kt):
//   fun format(serverUrl: String?): String? {
//       if (serverUrl.isNullOrBlank()) return null
//       var trimmed = serverUrl.trim().removeSuffix("/").removePrefix("http://").removePrefix("https://")
//       // If it's just a domain, add https://
//       return "https://$trimmed"
//   }

std::string formatServerUrl(const std::string& userInput) {
    if (userInput.empty()) return "";

    // Trim whitespace — Original Kotlin: serverUrl.trim()
    std::string url = userInput;
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.erase(0, 1);
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) url.pop_back();
    if (url.empty()) return "";

    // Remove trailing slash — Original Kotlin: removeSuffix("/")
    while (!url.empty() && url.back() == '/') url.pop_back();

    // Remove existing scheme if present — Original Kotlin: removePrefix("http://").removePrefix("https://")
    if (url.find("https://") == 0) url = url.substr(8);
    else if (url.find("http://") == 0) url = url.substr(7);

    // Re-add https:// scheme — Original Kotlin: return "https://$trimmed"
    return "https://" + url;
}

// ---- Server name extraction ----
// Original Kotlin (MatrixPatterns.kt):
//   fun extractServerName(mxid: String): String? {
//       val idx = mxid.lastIndexOf(':')
//       if (idx < 0) return null
//       return mxid.substring(idx + 1)
//   }

std::string extractServerName(const std::string& mxid) {
    auto colonPos = mxid.rfind(':');
    if (colonPos == std::string::npos) return "";
    return mxid.substr(colonPos + 1);
}

// ---- Well-known URL builder (defined in login_utils.cpp) ----
// Original Kotlin:
//   fun buildWellKnownUrl(domain: String): String {
//       return "https://$domain/.well-known/matrix/client"
//   }

// ---- URL validation ----
// Original Kotlin:
//   fun isValidHomeserverUrl(url: String?): Boolean {
//       if (url.isNullOrBlank()) return false
//       return url.startsWith("https://") && !url.contains("?")
//   }

bool isValidHomeserverUrl(const std::string& url) {
    if (url.empty()) return false;
    // Must use HTTPS — Matrix spec requirement
    // Exception: localhost for development
    if (url.find("https://") != 0) {
        if (url.find("http://localhost") == 0 || url.find("http://127.0.0.1") == 0 ||
            url.find("http://10.") == 0 || url.find("http://192.168.") == 0) {
            return true; // Allow local dev
        }
        return false;
    }
    // Must not contain query parameters in the base URL
    if (url.find('?') != std::string::npos) return false;
    return true;
}

bool isValidIdentityServerUrl(const std::string& url) {
    if (url.empty()) return true; // identity server is optional
    return url.find("https://") == 0;
}

// ---- JSON serialization ----
std::string wellKnownToJson(const ServerDiscoveryResult& result) {
    auto esc = [](const std::string& s) -> std::string {
        std::string out;
        for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
        return out;
    };
    std::ostringstream json;
    json << R"({"isValid": )" << (result.isValid ? "true" : "false") << ",";
    json << R"("homeserverBaseUrl": ")" << esc(result.homeserverBaseUrl) << R"(",)";
    json << R"("identityServerUrl": ")" << esc(result.identityServerUrl) << R"(",)";
    json << R"("error": ")" << esc(result.error) << R"(")";
    json << "}";
    return json.str();
}

// ---- Heuristic: does the input need a well-known lookup? ----
// Original Kotlin (ServerUrlFormatter.kt):
//   fun needsWellKnown(url: String): Boolean {
//       return !url.contains("://") && !url.contains(":")
//   }
bool needsWellKnownLookup(const std::string& serverInput) {
    if (serverInput.empty()) return false;
    // If it already has a scheme, it's a direct URL
    if (serverInput.find("://") != std::string::npos) return false;
    // If it has a port (domain:8448), it's a direct URL
    // But we need to exclude the MXID colon separator
    // e.g. "@user:matrix.org" should not trigger well-known on the full string
    // But this function receives the server part only (e.g. "matrix.org:8448")
    // "matrix.org:8448" → has port → direct
    // "matrix.org" → no port → needs well-known
    if (serverInput.find(':') != std::string::npos) return false;
    return true;
}

} // namespace progressive
