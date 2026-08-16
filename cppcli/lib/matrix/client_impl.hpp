// client_impl.hpp — the Client::Impl struct shared by the client TUs.
#pragma once

#include "client.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace matrixcli { namespace matrix {
struct Client::Impl {
    http::Client http;
    Credentials creds;
    std::string homeserver_url;
    std::string next_batch;
    std::atomic<bool> logged_in{false};
    std::atomic<bool> syncing{false};
    std::atomic<int> timeout{30};
    std::thread sync_thread;
    db::Database* db = nullptr;
    std::unique_ptr<e2ee::CryptoManager> crypto;
    std::map<std::string, bool> encrypted_rooms;
    PushRules pushRules;
    std::map<std::string, std::string> directChats;
    std::map<std::string, std::vector<std::string>> spaceChildren; // space_id -> [room_ids]
    std::map<std::string, std::string> spaceParents; // room_id -> space_id
    bool pushRulesLoaded = false;
};

}} // namespace matrixcli::matrix
