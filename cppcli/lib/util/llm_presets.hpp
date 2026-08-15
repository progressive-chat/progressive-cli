#pragma once

// The canonical LLM provider preset table (opencode-style). Used by the
// TUI first-run setup wizard, the agent-code provider presets and the
// Matrix agent config resolution. Keep this the single source of truth.

#include <string>
#include <vector>

namespace matrixcli { namespace util {

struct LlmPreset {
    std::string name;      // the short preset name ("deepseek", "mimo", ...)
    std::string provider;  // the wire format: "openai" | "anthropic"
    std::string endpoint;  // the API base URL
    std::string model;     // the default model
    bool local = false;    // local servers: the API key is optional
};

const std::vector<LlmPreset>& llmPresets();
const LlmPreset* findLlmPreset(const std::string& name);

}} // namespace matrixcli::util
