#include "lemmy_client.hpp"
#include <stdexcept>

namespace matrixcli { namespace lemmy {

void LemmyClient::setInstance(const std::string& url) {
    _instance = url;
    if (_instance.back() == '/') _instance.pop_back();
}

std::string LemmyClient::buildUrl(const std::string& path) {
    return _instance + "/api/v3" + path;
}

json LemmyClient::get(const std::string& path, const std::map<std::string, std::string>& params) {
    std::string qs;
    for (auto& [k, v] : params) qs += (qs.empty() ? "?" : "&") + k + "=" + v;
    auto headers = std::map<std::string, std::string>{{"Content-Type", "application/json"}};
    if (!_jwt.empty()) headers["Authorization"] = "Bearer " + _jwt;
    auto resp = _http.get(buildUrl(path) + qs, headers);
    if (resp.status_code != 200) throw std::runtime_error("Lemmy HTTP " + std::to_string(resp.status_code));
    return json::parse(resp.body);
}

json LemmyClient::post(const std::string& path, const json& body) {
    auto headers = std::map<std::string, std::string>{{"Content-Type", "application/json"}};
    if (!_jwt.empty()) headers["Authorization"] = "Bearer " + _jwt;
    auto resp = _http.post(buildUrl(path), body.dump(), headers);
    if (resp.status_code != 200) throw std::runtime_error("Lemmy HTTP " + std::to_string(resp.status_code));
    return json::parse(resp.body);
}

bool LemmyClient::login(const std::string& username, const std::string& password) {
    try {
        auto j = post("/user/login", {{"username_or_email", username}, {"password", password}});
        if (j.contains("jwt")) { _jwt = j["jwt"].get<std::string>(); return true; }
    } catch (...) {}
    return false;
}

std::vector<LemmyCommunity> LemmyClient::listCommunities(const std::string& type, int limit) {
    std::vector<LemmyCommunity> result;
    try {
        auto j = get("/community/list", {{"type_", type}, {"limit", std::to_string(limit)}});
        for (auto& c : j.value("communities", json::array())) {
            auto& cv = c["community"];
            LemmyCommunity lc;
            lc.id = cv["id"].get<int>();
            lc.name = cv.value("name", "");
            lc.title = cv.value("title", "");
            lc.description = cv.value("description", "");
            lc.subscribers = cv.value("subscribers", 0);
            result.push_back(lc);
        }
    } catch (...) {}
    return result;
}

LemmyCommunity LemmyClient::getCommunity(const std::string& name) {
    try {
        auto j = get("/community", {{"name", name}});
        auto& cv = j["community_view"]["community"];
        LemmyCommunity lc;
        lc.id = cv["id"].get<int>(); lc.name = cv["name"]; lc.title = cv.value("title", "");
        return lc;
    } catch (...) { return {}; }
}

std::vector<LemmyPost> LemmyClient::listPosts(const std::string& community_name,
                                                const std::string& sort, int limit) {
    std::vector<LemmyPost> result;
    try {
        std::map<std::string, std::string> params = {{"sort", sort}, {"limit", std::to_string(limit)}};
        if (!community_name.empty()) params["community_name"] = community_name;
        auto j = get("/post/list", params);
        for (auto& p : j.value("posts", json::array())) {
            auto& pv = p["post"];
            LemmyPost lp;
            lp.id = pv["id"].get<int>();
            lp.name = pv.value("name", "");
            lp.title = pv.value("name", pv.value("title", ""));
            lp.body = pv.value("body", "");
            lp.url = pv.value("url", "");
            lp.score = p["counts"].value("score", 0);
            lp.upvotes = p["counts"].value("upvotes", 0);
            lp.downvotes = p["counts"].value("downvotes", 0);
            lp.comment_count = p["counts"].value("comments", 0);
            lp.creator_name = p["creator"].value("name", "");
            lp.community_name = p["community"].value("name", "");
            result.push_back(lp);
        }
    } catch (...) {}
    return result;
}

LemmyPost LemmyClient::getPost(int post_id) {
    try {
        auto j = get("/post", {{"id", std::to_string(post_id)}});
        auto& pv = j["post_view"]["post"];
        LemmyPost lp; lp.id = pv["id"].get<int>(); lp.title = pv.value("name", "");
        lp.body = pv.value("body", ""); return lp;
    } catch (...) { return {}; }
}

int LemmyClient::createPost(const std::string& community_name, const std::string& title,
                              const std::string& body, const std::string& url) {
    try {
        int comm_id = getCommunity(community_name).id;
        json req = {{"community_id", comm_id}, {"name", title}};
        if (!body.empty()) req["body"] = body;
        if (!url.empty()) req["url"] = url;
        auto j = post("/post", req);
        return j["post_view"]["post"]["id"].get<int>();
    } catch (...) { return 0; }
}

bool LemmyClient::likePost(int post_id, int score) {
    try {
        post("/post/like", {{"post_id", post_id}, {"score", score}});
        return true;
    } catch (...) { return false; }
}

std::vector<LemmyComment> LemmyClient::listComments(int post_id, int limit) {
    std::vector<LemmyComment> result;
    try {
        auto j = get("/comment/list", {{"post_id", std::to_string(post_id)}, {"limit", std::to_string(limit)}, {"sort", "Hot"}});
        for (auto& c : j.value("comments", json::array())) {
            auto& cv = c["comment"];
            LemmyComment lc;
            lc.id = cv["id"].get<int>();
            lc.content = cv.value("content", "");
            lc.post_id = cv.value("post_id", 0);
            lc.score = c["counts"].value("score", 0);
            lc.creator_name = c["creator"].value("name", "");
            result.push_back(lc);
        }
    } catch (...) {}
    return result;
}

int LemmyClient::createComment(int post_id, const std::string& content, int parent_id) {
    try {
        json req = {{"post_id", post_id}, {"content", content}};
        if (parent_id > 0) req["parent_id"] = parent_id;
        auto j = post("/comment", req);
        return j["comment_view"]["comment"]["id"].get<int>();
    } catch (...) { return 0; }
}

}} // namespace matrixcli::lemmy
