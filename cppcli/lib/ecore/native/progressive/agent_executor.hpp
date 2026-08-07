#ifndef PROGRESSIVE_AGENT_EXECUTOR_HPP
#define PROGRESSIVE_AGENT_EXECUTOR_HPP

#include <string>
#include <vector>
#include <functional>

namespace progressive {

// ---- AI Agent Executor ----
// Implements an agentic LLM loop for Matrix chat automation.
// Inspired by Claude Code / Cursor / Copilot agent patterns.
//
// Activated via: /agent <task description>
// Example: /agent find messages from Alice about the future
//
// The agent loop:
//   1. Build system prompt with available tools + room context
//   2. Send task + context to LLM
//   3. Parse LLM response: text answer OR tool_call request
//   4. If tool_call: execute the tool, feed result back to LLM
//   5. Repeat until task complete or max iterations (default 10)
//
// Tools available to the LLM (declared in the system prompt):
//   read_messages(room_id, limit, before_event_id)
//   send_message(room_id, text)
//   edit_message(room_id, event_id, new_text)
//   search_messages(room_id, query)
//   list_users(room_id)
//   get_user_info(user_id)
//   react_to_message(room_id, event_id, emoji)
//   send_direct_message(user_id, text)
//   get_room_info(room_id)

// ==== Agent Configuration ====

struct AgentConfig {
    std::string systemPrompt;      // LLM system prompt (role + tools + rules)
    std::string toolsDescription;  // JSON Schema of available tools
    int maxIterations = 10;        // safety limit — prevent infinite loops
    int maxContextMessages = 50;   // how many recent messages to include
    bool verbose = false;          // show intermediate steps
};

// ==== Tool Call Definition ====

struct AgentToolCall {
    std::string toolName;          // "read_messages", "send_message", etc.
    std::string argumentsJson;     // JSON object with tool parameters
    std::string callId;            // unique ID for this call
};

// ==== Agent State ====

struct AgentState {
    // Input
    std::string task;              // user's task description
    std::string roomId;            // current room
    std::string userId;            // requesting user
    std::string roomContext;       // recent messages + room info

    // Progress
    int iteration = 0;             // current iteration number
    std::string llmResponse;       // latest raw LLM response
    std::string finalAnswer;       // final text answer for the user
    std::vector<AgentToolCall> pendingToolCalls; // tools to execute

    // History
    std::vector<std::string> conversationHistory; // all prompts + responses

    // Status
    bool isComplete = false;       // agent finished the task
    bool hasError = false;         // error occurred
    std::string errorMessage;
};

// ==== Prompt Builder ====

// Build the initial system prompt with tools and room context.
// This is the first message sent to the LLM to set up the agent.
std::string buildAgentSystemPrompt(const AgentConfig& config);

// Build the user prompt for a given iteration.
// Includes: task + conversation history + latest tool results.
std::string buildAgentUserPrompt(const AgentState& state, const AgentConfig& config);

// Format recent messages as context for the LLM.
// "Alice (2024-01-15 10:30): I think the future of Matrix is..."
std::string formatMessagesForAgent(
    const std::vector<std::string>& senders,
    const std::vector<std::string>& bodies,
    const std::vector<int64_t>& timestamps
);

// ==== Tool Call Parser ====

// Parse tool calls from LLM response text.
// Supports JSON format: {"tool_calls": [{"name": "...", "arguments": {...}}]}
// Supports markdown format: ```tool\nread_messages(...)\n```
std::vector<AgentToolCall> parseToolCalls(const std::string& llmResponse);

// Check if the LLM response contains tool calls (vs a final answer).
bool hasToolCalls(const std::string& llmResponse);

// Extract the text answer from an LLM response (ignore tool call blocks).
std::string extractTextAnswer(const std::string& llmResponse);

// ==== Agent Loop ====

// Process one iteration of the agent loop.
// Given the current state + LLM response, returns updated state.
// The Kotlin layer handles: LLM API call → C++ parses response → Kotlin executes tools
AgentState processAgentIteration(const AgentState& state, const std::string& llmRawResponse);

// Build the tool call result text to feed back to LLM.
std::string buildToolResultText(const std::string& toolName, const std::string& resultJson);

// Check if the agent should stop (final answer or max iterations).
bool shouldStopAgent(const AgentState& state, const AgentConfig& config);

// ==== Tool Schemas (for system prompt) ====

// Get the JSON Schema for all available tools.
// This is injected into the system prompt so the LLM knows what it can do.
std::string getAgentToolsSchema();

// ==== Serialization ====

std::string agentStateToJson(const AgentState& state);

} // namespace progressive

#endif // PROGRESSIVE_AGENT_EXECUTOR_HPP
