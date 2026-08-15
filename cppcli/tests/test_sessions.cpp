// The unit tests for the LLM session store: the paths, the name
// sanitization and the --fresh archiving (the "never destroy" promise).
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../lib/util/llm_sessions.hpp"

using matrixcli::util::archiveSession;
using matrixcli::util::llmSessionPathFor;
using matrixcli::util::llmSessionsDir;

namespace fs = std::filesystem;

static std::vector<std::string> sessionFiles() {
    std::vector<std::string> out;
    if (!fs::exists(llmSessionsDir())) return out;
    for (const auto& e : fs::directory_iterator(llmSessionsDir())) {
        const std::string fn = e.path().filename().string();
        if (fn.starts_with("llm-") && fn.ends_with(".json"))
            out.push_back(fn);
    }
    return out;
}

int main() {
    const fs::path dir = "/tmp/matrixcli_test_sessions";
    fs::remove_all(dir);
    fs::create_directories(dir);
    setenv("XDG_DATA_HOME", dir.c_str(), 1);

    // 1. The default session name is "chat".
    assert(llmSessionPathFor("") ==
           dir.string() + "/matrixcli/sessions/llm-chat.json");
    // 2. The named session.
    assert(llmSessionPathFor("work") ==
           dir.string() + "/matrixcli/sessions/llm-work.json");
    // 3. The sanitization: only [a-zA-Z0-9._-] survives.
    assert(llmSessionPathFor("a/b c\\d") ==
           dir.string() + "/matrixcli/sessions/llm-a_b_c_d.json");

    // 4. The --fresh archiving: the old file is renamed, never deleted.
    {
        fs::create_directories(llmSessionsDir());
        std::ofstream(llmSessionPathFor("chat")) << "old turns";
        archiveSession("chat");
        assert(!fs::exists(llmSessionPathFor("chat")));
        auto files = sessionFiles();
        assert(files.size() == 1);
        assert(files[0].starts_with("llm-chat-"));
        assert(files[0].ends_with(".json"));
    }
    // 5. Archiving an empty store is a no-op (no throw, no file).
    {
        archiveSession("nothing");
        auto files = sessionFiles();
        assert(files.size() == 1);  // still only the archived chat
    }

    fs::remove_all(dir);
    std::printf("test_sessions: all ok\n");
    return 0;
}
