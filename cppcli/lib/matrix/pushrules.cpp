#include "pushrules.hpp"
#include "../util/logger.hpp"

#include <algorithm>
#include <fnmatch.h>

namespace matrixcli { namespace matrix {

PushRule PushRule::fromJson(const json& j) {
    PushRule r;
    r.rule_id = j.value("rule_id", "");
    r.kind = j.value("kind", "override");
    r.enabled = j.value("enabled", true);
    r.pattern = j.value("pattern", "");
    for (auto& a : j.value("actions", json::array())) {
        PushRuleAction act;
        if (a.is_string()) {
            act.kind = a.get<std::string>();
        } else if (a.is_object()) {
            act.kind = a.value("kind", "");
            act.tweak = a.value("tweak", "");
            act.value = a.value("value", "");
        }
        r.actions.push_back(act);
    }
    return r;
}

void PushRules::load(const std::string& rulesJson) {
    try {
        auto j = json::parse(rulesJson);
        auto loadSection = [&](const std::string& key, std::vector<PushRule>& target) {
            if (j.contains(key)) {
                for (auto& rj : j[key]) target.push_back(PushRule::fromJson(rj));
            }
        };
        loadSection("global.override", _overrides);
        loadSection("global.content", _content);
        loadSection("global.room", _room);
        loadSection("global.sender", _sender);
        loadSection("global.underride", _underrides);
    } catch (...) {
        initDefaults();
    }
}

void PushRules::addRule(const PushRule& rule) {
    if (rule.kind == "override") _overrides.push_back(rule);
    else if (rule.kind == "content") _content.push_back(rule);
    else if (rule.kind == "room") _room.push_back(rule);
    else if (rule.kind == "sender") _sender.push_back(rule);
    else _underrides.push_back(rule);
}

void PushRules::initDefaults() {
    // Default underride: 1:1 room
    _underrides.push_back({"", "underride", true, "", ".m.rule.room_one_to_one",
        {{PushRuleAction{"notify"}, PushRuleAction{"set_tweak", "sound", "default"}}}});
    // Encrypted room
    _underrides.push_back({"", "underride", true, "", ".m.rule.encrypted_room_one_to_one",
        {{PushRuleAction{"notify"}, PushRuleAction{"set_tweak", "sound", "default"}}}});
    // All other messages
    _underrides.push_back({"", "underride", true, "", ".m.rule.message",
        {{PushRuleAction{"notify"}}}});
    // Tombstone
    _underrides.push_back({"", "underride", true, "", ".m.rule.tombstone",
        {{PushRuleAction{"notify"}, PushRuleAction{"set_tweak", "highlight", "true"}}}});
}

bool PushRules::matchRule(const PushRule& rule, const json& event) {
    if (!rule.enabled) return false;
    if (rule.pattern.empty()) return true;

    // Check body content for glob match
    std::string body = event.value("content", json::object()).value("body", "");
    if (body.empty()) return false;

    // Simple glob matching: * matches anything
    std::string pattern = rule.pattern;
    // fnmatch for full pattern support
    return fnmatch(pattern.c_str(), body.c_str(), FNM_CASEFOLD) == 0;
}

PushResult PushRules::evaluate(const json& event, int roomMemberCount) {
    PushResult result;
    result.notify = true; // default: notify

    // Priority: override > content > room > sender > underride
    auto process = [&](const std::vector<PushRule>& rules) -> bool {
        for (auto& rule : rules) {
            if (!matchRule(rule, event)) continue;
            for (auto& act : rule.actions) {
                if (act.kind == "dont_notify") {
                    result.notify = false;
                    return true; // stop processing
                }
                if (act.kind == "notify") result.notify = true;
                if (act.tweak == "highlight") result.highlight = true;
                if (act.tweak == "sound") result.sound = act.value;
            }
            result.ruleId = rule.rule_id;
            if (rule.kind != "underride") return true; // stop after first match (except underride)
        }
        return false;
    };

    process(_overrides);
    process(_content);
    process(_room);
    process(_sender);
    process(_underrides);

    return result;
}

}} // namespace matrixcli::matrix
