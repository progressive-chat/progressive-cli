#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace matrixcli { namespace matrix {

using json = nlohmann::json;

struct PushRuleAction {
    std::string kind; // "notify", "dont_notify", "coalesce", "set_tweak"
    std::string tweak; // "sound", "highlight"
    std::string value;
};

struct PushRule {
    std::string id;
    std::string kind; // "override", "underride", "content", "room", "sender"
    bool enabled = true;
    std::string pattern; // glob pattern
    std::string rule_id;
    std::vector<PushRuleAction> actions;

    static PushRule fromJson(const json& j);
};

// Evaluation result for a single event
struct PushResult {
    bool notify = true;
    bool highlight = false;
    std::string sound;
    std::string ruleId;
};

class PushRules {
public:
    void load(const std::string& rulesJson); // from GET /pushrules/
    void addRule(const PushRule& rule);

    PushResult evaluate(const json& event, int roomMemberCount = -1);

private:
    bool matchRule(const PushRule& rule, const json& event);

    std::vector<PushRule> _overrides;
    std::vector<PushRule> _content;
    std::vector<PushRule> _room;
    std::vector<PushRule> _sender;
    std::vector<PushRule> _underrides;

    // Default rules
    void initDefaults();
};

}} // namespace matrixcli::matrix
