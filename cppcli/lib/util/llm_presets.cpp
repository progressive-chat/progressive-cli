#include "llm_presets.hpp"

namespace matrixcli { namespace util {

const std::vector<LlmPreset>& llmPresets() {
    static const std::vector<LlmPreset> presets = {
        {"openai", "openai", "https://api.openai.com", "gpt-4o-mini", false},
        {"anthropic", "anthropic", "https://api.anthropic.com",
         "claude-3-5-haiku-20241022", false},
        {"deepseek", "openai", "https://api.deepseek.com", "deepseek-chat", false},
        {"qwen", "openai", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
         "qwen-plus", false},
        {"openrouter", "openai", "https://openrouter.ai/api/v1",
         "openai/gpt-4o-mini", false},
        {"groq", "openai", "https://api.groq.com/openai/v1",
         "llama-3.3-70b-versatile", false},
        {"fireworks", "openai", "https://api.fireworks.ai/inference/v1",
         "accounts/fireworks/models/llama-v3p1-8b-instruct", false},
        {"mimo", "openai", "https://platform.xiaomimimo.com/v1", "mimo-v2.5", false},
        {"ollama", "openai", "http://localhost:11434", "llama3.2", true},
        {"lmstudio", "openai", "http://localhost:1234", "local-model", true},
    };
    return presets;
}

const LlmPreset* findLlmPreset(const std::string& name) {
    for (const auto& p : llmPresets()) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

}} // namespace matrixcli::util
