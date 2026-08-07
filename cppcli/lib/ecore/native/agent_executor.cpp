#include "progressive/agent_executor.hpp"
#include "progressive/string_utils.hpp"
#include <sstream>
#include <algorithm>

namespace progressive {

// ==== System Prompt Builder ====
// Constructs the system prompt that defines the agent's role, tools, and rules.

std::string buildAgentSystemPrompt(const AgentConfig& config) {
    std::ostringstream prompt;

    prompt << R"(You are a helpful Matrix chat assistant. You help users find information, summarize conversations, )";
    prompt << R"(search for messages, and perform actions in their Matrix rooms.)" << "\n\n";

    prompt << R"(## Rules)" << "\n";
    prompt << R"(- Always respond with either a text answer OR a tool call, never both in the same response.)" << "\n";
    prompt << R"(- When you need information you don't have, use a tool call to retrieve it.)" << "\n";
    prompt << R"(- Be concise. Users are reading on a chat interface, not a web page.)" << "\n";
    prompt << R"(- If you cannot complete the task, explain why clearly.)" << "\n";
    prompt << R"(- Never make up information. If you're unsure, use search_messages to verify.)" << "\n";
    prompt << R"(- When sending messages, keep formatting minimal (basic markdown only).)" << "\n\n";

    prompt << R"(## Available Tools)" << "\n";
    prompt << R"(When you need to perform an action, respond with a JSON tool call:)" << "\n\n";
    prompt << R"(```json)" << "\n";
    prompt << R"({"tool_calls": [{"name": "<tool_name>", "arguments": {<params>}}]})" << "\n";
    prompt << R"(```)" << "\n\n";

    prompt << config.toolsDescription << "\n\n";

    prompt << R"(## Tool Usage Examples)" << "\n";
    prompt << R"(To read the last 20 messages:)" << "\n";
    prompt << R"({"tool_calls": [{"name": "read_messages", "arguments": {"room_id": "<room>", "limit": 20}}]})" << "\n\n";
    prompt << R"(To search for messages about a topic:)" << "\n";
    prompt << R"({"tool_calls": [{"name": "search_messages", "arguments": {"room_id": "<room>", "query": "quantum computing"}}]})" << "\n\n";
    prompt << R"(To send a message:)" << "\n";
    prompt << R"({"tool_calls": [{"name": "send_message", "arguments": {"room_id": "<room>", "text": "Hello everyone!"}}]})" << "\n\n";

    if (!config.systemPrompt.empty()) {
        prompt << config.systemPrompt << "\n";
    }

    return prompt.str();
}

// ==== User Prompt Builder ====

std::string buildAgentUserPrompt(const AgentState& state, const AgentConfig& config) {
    std::ostringstream prompt;

    if (state.iteration == 0) {
        // First iteration: introduce the task
        prompt << R"(## Room Context)" << "\n";
        prompt << state.roomContext << "\n\n";
        prompt << R"(## Task)" << "\n";
        prompt << state.task << "\n";
    } else {
        // Subsequent iterations: tool results + continue
        prompt << R"(## Tool Results)" << "\n";
        for (size_t i = 0; i < state.conversationHistory.size(); ++i) {
            if (i >= state.conversationHistory.size() - 2) {
                prompt << state.conversationHistory[i] << "\n";
            }
        }
        prompt << "\nContinue working on the task: " << state.task << "\n";
    }

    return prompt.str();
}

// ==== Message Formatter ====

std::string formatMessagesForAgent(
    const std::vector<std::string>& senders,
    const std::vector<std::string>& bodies,
    const std::vector<int64_t>& timestamps)
{
    std::ostringstream out;
    size_t count = std::min({senders.size(), bodies.size(), timestamps.size()});

    out << "Recent messages:\n";
    for (size_t i = 0; i < count; ++i) {
        // Format timestamp as readable
        time_t ts = static_cast<time_t>(timestamps[i] / 1000);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", localtime(&ts));

        out << "[" << timeBuf << "] " << senders[i] << ": " << bodies[i] << "\n";
    }
    return out.str();
}

// ==== Tool Call Parser ====
// Parses tool_calls JSON from LLM response.
// Format: {"tool_calls": [{"name": "read_messages", "arguments": {"room_id": "...", "limit": 20}}]}

std::vector<AgentToolCall> parseToolCalls(const std::string& llmResponse) {
    std::vector<AgentToolCall> calls;

    // Find "tool_calls" key
    auto tcPos = llmResponse.find("\"tool_calls\"");
    if (tcPos == std::string::npos) return calls;

    // Find the array of tool calls
    auto arrayStart = llmResponse.find('[', tcPos);
    if (arrayStart == std::string::npos) return calls;

    // Parse each tool call object
    size_t pos = arrayStart + 1;
    while (pos < llmResponse.size()) {
        auto objStart = llmResponse.find('{', pos);
        if (objStart == std::string::npos || objStart >= llmResponse.find(']', arrayStart)) break;

        int braceDepth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < llmResponse.size() && braceDepth > 0) {
            if (llmResponse[objEnd] == '{') braceDepth++;
            else if (llmResponse[objEnd] == '}') braceDepth--;
            objEnd++;
        }

        std::string callJson = llmResponse.substr(objStart, objEnd - objStart);

        AgentToolCall call;
        // Extract call ID if present
        auto idPos = callJson.find("\"id\"");
        if (idPos != std::string::npos) {
            auto colon = callJson.find(':', idPos);
            if (colon != std::string::npos) {
                auto q = callJson.find('"', colon);
                if (q != std::string::npos) {
                    auto qe = callJson.find('"', q + 1);
                    if (qe != std::string::npos) {
                        call.callId = callJson.substr(q + 1, qe - q - 1);
                    }
                }
            }
        }

        // Extract name
        auto namePos = callJson.find("\"name\"");
        if (namePos != std::string::npos) {
            auto colon = callJson.find(':', namePos);
            if (colon != std::string::npos) {
                auto q = callJson.find('"', colon);
                if (q != std::string::npos) {
                    auto qe = callJson.find('"', q + 1);
                    if (qe != std::string::npos) {
                        call.toolName = callJson.substr(q + 1, qe - q - 1);
                    }
                }
            }
        }

        // Extract arguments (the whole nested JSON object)
        auto argsPos = callJson.find("\"arguments\"");
        if (argsPos != std::string::npos) {
            auto colon = callJson.find(':', argsPos);
            if (colon != std::string::npos) {
                auto braceStart = callJson.find('{', colon);
                if (braceStart != std::string::npos) {
                    int argDepth = 1;
                    size_t argEnd = braceStart + 1;
                    while (argEnd < callJson.size() && argDepth > 0) {
                        if (callJson[argEnd] == '{') argDepth++;
                        else if (callJson[argEnd] == '}') argDepth--;
                        argEnd++;
                    }
                    call.argumentsJson = callJson.substr(braceStart, argEnd - braceStart);
                }
            }
        }

        if (!call.toolName.empty()) {
            calls.push_back(call);
        }

        pos = objEnd;
    }

    return calls;
}

bool hasToolCalls(const std::string& llmResponse) {
    return llmResponse.find("\"tool_calls\"") != std::string::npos;
}

std::string extractTextAnswer(const std::string& llmResponse) {
    // If there are tool calls, extract only the text before them
    auto tcPos = llmResponse.find("\"tool_calls\"");
    if (tcPos != std::string::npos) {
        std::string before = llmResponse.substr(0, tcPos);
        // Try to extract just the "content" field if present
        auto contentPos = before.find("\"content\"");
        if (contentPos != std::string::npos) {
            auto colon = before.find(':', contentPos);
            if (colon != std::string::npos) {
                auto q = before.find('"', colon);
                if (q != std::string::npos) {
                    auto qe = before.find('"', q + 1);
                    if (qe != std::string::npos) {
                        return before.substr(q + 1, qe - q - 1);
                    }
                }
            }
        }
        return before;
    }

    // No tool calls — the whole response is the text answer
    // Strip JSON wrapper if present
    auto contentPos = llmResponse.find("\"content\"");
    if (contentPos != std::string::npos) {
        auto colon = llmResponse.find(':', contentPos);
        if (colon != std::string::npos) {
            auto q = llmResponse.find('"', colon);
            if (q != std::string::npos) {
                auto qe = llmResponse.find('"', q + 1);
                if (qe != std::string::npos) {
                    return llmResponse.substr(q + 1, qe - q - 1);
                }
            }
        }
    }

    return llmResponse;
}

// ==== Agent Loop ====

AgentState processAgentIteration(const AgentState& state, const std::string& llmRawResponse) {
    AgentState updated = state;
    updated.llmResponse = llmRawResponse;
    updated.iteration++;

    // Store in conversation history
    updated.conversationHistory.push_back("User: " + state.task);
    updated.conversationHistory.push_back("Assistant: " + llmRawResponse);

    // Parse tool calls from LLM response
    updated.pendingToolCalls = parseToolCalls(llmRawResponse);

    if (!updated.pendingToolCalls.empty()) {
        // LLM wants to use tools — Kotlin will execute them
        updated.isComplete = false;
    } else {
        // No tool calls — this is the final answer
        updated.finalAnswer = extractTextAnswer(llmRawResponse);
        updated.isComplete = true;
    }

    return updated;
}

std::string buildToolResultText(const std::string& toolName, const std::string& resultJson) {
    std::ostringstream out;
    out << "Tool '" << toolName << "' returned:\n";
    out << resultJson << "\n\n";
    out << "Use this information to continue the task or provide the final answer.";
    return out.str();
}

bool shouldStopAgent(const AgentState& state, const AgentConfig& config) {
    if (state.hasError) return true;
    if (state.isComplete) return true;
    if (state.iteration >= config.maxIterations) return true;
    return false;
}

// ==== Tool Schema ====

std::string getAgentToolsSchema() {
    std::ostringstream schema;
    schema << "### read_messages\n";
    schema << "Read recent messages from a room.\n";
    schema << "Arguments: room_id, limit (default 20), before_event_id (optional)\n";
    schema << "Returns: array of {sender, body, timestamp, event_id}\n\n";

    schema << "### send_message\n";
    schema << "Send a text message to a room.\n";
    schema << "Arguments: room_id, text\n";
    schema << "Returns: {event_id}\n\n";

    schema << "### edit_message\n";
    schema << "Edit a previously sent message.\n";
    schema << "Arguments: room_id, event_id, new_text\n";
    schema << "Returns: {success}\n\n";

    schema << "### search_messages\n";
    schema << "Search messages in a room for a query string.\n";
    schema << "Arguments: room_id, query\n";
    schema << "Returns: array of {sender, body, timestamp, event_id, score}\n\n";

    schema << "### list_users\n";
    schema << "List members of a room.\n";
    schema << "Arguments: room_id\n";
    schema << "Returns: array of {user_id, display_name, membership}\n\n";

    schema << "### get_user_info\n";
    schema << "Get information about a specific user.\n";
    schema << "Arguments: user_id\n";
    schema << "Returns: {user_id, display_name, avatar_url, presence}\n\n";

    schema << "### react_to_message\n";
    schema << "Add a reaction emoji to a message.\n";
    schema << "Arguments: room_id, event_id, emoji\n";
    schema << "Returns: {success}\n\n";

    schema << "### get_room_info\n";
    schema << "Get information about a room.\n";
    schema << "Arguments: room_id\n";
    schema << "Returns: {name, topic, member_count, is_encrypted}\n\n";

    schema << "### send_direct_message\n";
    schema << "Send a direct message to a user.\n";
    schema << "Arguments: user_id, text\n";
    schema << "Returns: {room_id, event_id}";
    return schema.str();
}

// ==== Serialization ====

std::string agentStateToJson(const AgentState& state) {
    auto esc = [](const std::string& s) -> std::string {
        std::string out; return escapeJson(s); return out;
    };
    std::ostringstream json;
    json << R"({"task": ")" << esc(state.task) << R"(",)";
    json << R"("roomId": ")" << esc(state.roomId) << R"(",)";
    json << R"("iteration": )" << state.iteration << ",";
    json << R"("isComplete": )" << (state.isComplete ? "true" : "false") << ",";
    json << R"("hasError": )" << (state.hasError ? "true" : "false") << ",";
    json << R"("finalAnswer": ")" << esc(state.finalAnswer) << R"(",)";
    json << R"("errorMessage": ")" << esc(state.errorMessage) << R"(",)";
    json << R"("pendingToolCalls": [)";
    for (size_t i = 0; i < state.pendingToolCalls.size(); ++i) {
        if (i > 0) json << ",";
        json << R"({"name": ")" << esc(state.pendingToolCalls[i].toolName) << R"(")";
        json << R"(,"arguments": )" << state.pendingToolCalls[i].argumentsJson;
        json << R"(,"callId": ")" << esc(state.pendingToolCalls[i].callId) << R"("})";
    }
    json << "]}";
    return json.str();
}

} // namespace progressive
