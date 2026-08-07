#include "progressive/login_flow.hpp"
#include <sstream>

namespace progressive {

// ---- Login Flow Parsing ----
// Original Kotlin (LoginWizard.kt):
//   suspend fun getLoginFlows(): LoginFlows {
//       val params = LoginParams(homeServerConnectionConfig)
//       val response = executeRequest { authAPI.getLoginFlows() }
//       val flows = response.flows.map {
//           LoginFlow(
//               type = it.type,
//               identityProviders = it.identityProviders?.map { idp ->
//                   SsoIdentityProvider(idp.id, idp.name, idp.brand, idp.icon)
//               }.orEmpty()
//           )
//       }
//       return LoginFlows(flows)
//   }

LoginAuthFlowsResult parseLoginFlows(const std::string& json) {
    LoginAuthFlowsResult result;

    // Find the "flows" array
    auto flowsPos = json.find("\"flows\"");
    if (flowsPos == std::string::npos) {
        result.error = "Missing 'flows' array in login response";
        return result;
    }

    auto arrayStart = json.find('[', flowsPos);
    if (arrayStart == std::string::npos) {
        result.error = "Invalid 'flows' format — expected JSON array";
        return result;
    }

    // Parse each flow object within the array
    // Each flow: {"type": "m.login.password", "identity_providers": [...]}
    size_t pos = arrayStart + 1;
    int arrayDepth = 1;
    std::string currentFlow;

    while (pos < json.size() && arrayDepth > 0) {
        if (json[pos] == '{') {
            // Start of a flow object
            int braceDepth = 1;
            size_t flowStart = pos;
            pos++;
            while (pos < json.size() && braceDepth > 0) {
                if (json[pos] == '{') braceDepth++;
                else if (json[pos] == '}') braceDepth--;
                pos++;
            }
            currentFlow = json.substr(flowStart, pos - flowStart);

            // Parse the flow type
            LoginAuthFlow flow;
            auto typePos = currentFlow.find("\"type\"");
            if (typePos != std::string::npos) {
                auto colon = currentFlow.find(':', typePos);
                if (colon != std::string::npos) {
                    auto quote = currentFlow.find('"', colon);
                    if (quote != std::string::npos) {
                        auto endQuote = currentFlow.find('"', quote + 1);
                        if (endQuote != std::string::npos) {
                            flow.rawType = currentFlow.substr(quote + 1, endQuote - quote - 1);
                        }
                    }
                }
            }

            // Map raw type string to enum
            if (flow.rawType == "m.login.password") {
                flow.type = LoginFlowType::Password;
                result.hasPassword = true;
            } else if (flow.rawType == "m.login.sso" || flow.rawType == "m.login.cas") {
                flow.type = LoginFlowType::Sso;
                result.hasSso = true;
                // Parse SSO providers if present
                flow.ssoProviders = parseSsoProviders(currentFlow);
            } else if (flow.rawType == "m.login.token") {
                flow.type = LoginFlowType::Token;
                result.hasToken = true;
            } else if (flow.rawType == "m.login.dummy") {
                flow.type = LoginFlowType::Dummy;
            } else if (flow.rawType == "m.login.email.code") {
                flow.type = LoginFlowType::EmailCode;
            } else if (flow.rawType == "m.login.email.url") {
                flow.type = LoginFlowType::EmailUrl;
            } else if (flow.rawType == "m.login.msisdn") {
                flow.type = LoginFlowType::PhoneCode;
            } else if (flow.rawType == "m.login.recaptcha") {
                flow.type = LoginFlowType::Recaptcha;
            } else if (flow.rawType == "m.login.terms") {
                flow.type = LoginFlowType::Terms;
            }

            result.flows.push_back(flow);
        } else if (json[pos] == '[') {
            arrayDepth++;
            pos++;
        } else if (json[pos] == ']') {
            arrayDepth--;
            pos++;
        } else {
            pos++;
        }
    }

    result.isValid = !result.flows.empty();
    if (!result.isValid) {
        result.error = "No valid login flows found";
    }

    return result;
}

std::vector<SsoProvider> parseSsoProviders(const std::string& flowJson) {
    std::vector<SsoProvider> providers;

    // Original Kotlin (LoginWizard.kt):
    //   flow.identityProviders?.map {
    //       SsoIdentityProvider(id = it.id, name = it.name, brand = it.brand, icon = it.icon)
    //   }
    //
    // JSON format: "identity_providers": [{"id": "google", "name": "Google", ...}]

    auto providersPos = flowJson.find("\"identity_providers\"");
    if (providersPos == std::string::npos) return providers;

    auto arrayStart = flowJson.find('[', providersPos);
    if (arrayStart == std::string::npos) return providers;

    auto arrayEnd = flowJson.find(']', arrayStart);
    if (arrayEnd == std::string::npos) return providers;

    // Extract each provider object
    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        auto objStart = flowJson.find('{', pos);
        if (objStart == std::string::npos || objStart >= arrayEnd) break;

        int braceDepth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < arrayEnd && braceDepth > 0) {
            if (flowJson[objEnd] == '{') braceDepth++;
            else if (flowJson[objEnd] == '}') braceDepth--;
            objEnd++;
        }

        std::string providerJson = flowJson.substr(objStart, objEnd - objStart);

        SsoProvider provider;
        // Extract fields
        auto extractStr = [&](const std::string& field) -> std::string {
            auto search = "\"" + field + "\":\"";
            auto p = providerJson.find(search);
            if (p == std::string::npos) return "";
            p += search.size();
            auto end = providerJson.find('"', p);
            if (end == std::string::npos) return "";
            return providerJson.substr(p, end - p);
        };

        provider.id = extractStr("id");
        provider.name = extractStr("name");
        provider.brand = extractStr("brand");
        provider.iconUrl = extractStr("icon");

        if (!provider.id.empty()) {
            providers.push_back(provider);
        }

        pos = objEnd;
    }

    return providers;
}

bool requiresIdentityServer(LoginFlowType type) {
    return type == LoginFlowType::Sso || type == LoginFlowType::EmailCode ||
           type == LoginFlowType::PhoneCode || type == LoginFlowType::Token;
}

std::string loginFlowTypeToName(LoginFlowType type) {
    switch (type) {
        case LoginFlowType::Password: return "Password";
        case LoginFlowType::Sso: return "Single Sign-On";
        case LoginFlowType::Token: return "Login Token";
        case LoginFlowType::Dummy: return "Test/Dummy";
        case LoginFlowType::EmailCode: return "Email Code";
        case LoginFlowType::EmailUrl: return "Email Link";
        case LoginFlowType::PhoneCode: return "Phone Code";
        case LoginFlowType::Recaptcha: return "CAPTCHA";
        case LoginFlowType::Terms: return "Terms of Service";
        default: return "Unknown";
    }
}

std::string loginFlowTypeDescription(LoginFlowType type) {
    switch (type) {
        case LoginFlowType::Password:
            return "Sign in with your Matrix username and password";
        case LoginFlowType::Sso:
            return "Sign in using a third-party identity provider (Google, GitHub, etc.)";
        case LoginFlowType::Token:
            return "Sign in using a login token from SSO callback";
        case LoginFlowType::EmailCode:
            return "Sign in using a verification code sent to your email";
        case LoginFlowType::EmailUrl:
            return "Sign in by clicking a link sent to your email";
        case LoginFlowType::PhoneCode:
            return "Sign in using a verification code sent via SMS";
        case LoginFlowType::Recaptcha:
            return "Complete a CAPTCHA challenge";
        case LoginFlowType::Terms:
            return "Accept the server's terms of service";
        default:
            return "Unknown login method";
    }
}

std::string getSsoProviderIcon(const std::string& providerId) {
    // Known SSO providers with icon mappings
    if (providerId == "google") return "ic_sso_google";
    if (providerId == "github") return "ic_sso_github";
    if (providerId == "apple") return "ic_sso_apple";
    if (providerId == "facebook") return "ic_sso_facebook";
    if (providerId == "gitlab") return "ic_sso_gitlab";
    if (providerId == "twitter") return "ic_sso_twitter";
    if (providerId == "microsoft") return "ic_sso_microsoft";
    if (providerId == "saml") return "ic_sso_saml";
    if (providerId == "oidc") return "ic_sso_oidc";
    if (providerId == "cas") return "ic_sso_cas";
    return "ic_sso_default";
}

std::vector<std::string> getSupportedLoginTypes() {
    return {
        "m.login.password",
        "m.login.sso",
        "m.login.token",
        "m.login.email.code",
        "m.login.email.url",
        "m.login.msisdn",
        "m.login.recaptcha",
        "m.login.terms",
        "m.login.dummy"
    };
}

std::string loginFlowsToJson(const LoginAuthFlowsResult& result) {
    auto esc = [](const std::string& s) -> std::string {
        std::string out;
        for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
        return out;
    };
    std::ostringstream json;
    json << R"({"isValid": )" << (result.isValid ? "true" : "false") << ",";
    json << R"("hasPassword": )" << (result.hasPassword ? "true" : "false") << ",";
    json << R"("hasSso": )" << (result.hasSso ? "true" : "false") << ",";
    json << R"("hasToken": )" << (result.hasToken ? "true" : "false") << ",";
    json << R"("error": ")" << esc(result.error) << R"(",)";
    json << R"("flows": [)";
    for (size_t i = 0; i < result.flows.size(); ++i) {
        if (i > 0) json << ",";
        const auto& f = result.flows[i];
        json << "{";
        json << R"("type": ")" << f.rawType << R"(",)";
        json << R"("name": ")" << esc(loginFlowTypeToName(f.type)) << R"(",)";
        json << R"("description": ")" << esc(loginFlowTypeDescription(f.type)) << R"(",)";
        json << R"("ssoProviders": [)";
        for (size_t j = 0; j < f.ssoProviders.size(); ++j) {
            if (j > 0) json << ",";
            json << "{";
            json << R"("id": ")" << esc(f.ssoProviders[j].id) << R"(",)";
            json << R"("name": ")" << esc(f.ssoProviders[j].name) << R"(",)";
            json << R"("icon": ")" << esc(getSsoProviderIcon(f.ssoProviders[j].id)) << R"(")";
            json << "}";
        }
        json << "]}";
    }
    json << "]}";
    return json.str();
}

} // namespace progressive
