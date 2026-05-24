#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../http/http.hpp"

namespace matrixcli { namespace lemmy {

using json = nlohmann::json;

struct LemmyCommunity {
    int id = 0;
    std::string name;
    std::string title;
    std::string description;
    int subscribers = 0;
};

struct LemmyPost {
    int id = 0;
    std::string name;
    std::string title;
    std::string body;
    std::string url;
    int score = 0;
    int upvotes = 0;
    int downvotes = 0;
    int comment_count = 0;
    std::string creator_name;
    std::string community_name;
};

struct LemmyComment {
    int id = 0;
    std::string content;
    int post_id = 0;
    int score = 0;
    std::string creator_name;
};

class LemmyClient {
public:
    void setInstance(const std::string& url);
    bool login(const std::string& username, const std::string& password);
    bool isLoggedIn() const { return !_jwt.empty(); }

    // Communities
    std::vector<LemmyCommunity> listCommunities(const std::string& type = "All", int limit = 20);
    LemmyCommunity getCommunity(const std::string& name);

    // Posts
    std::vector<LemmyPost> listPosts(const std::string& community_name = "",
                                      const std::string& sort = "Hot", int limit = 20);
    LemmyPost getPost(int post_id);
    int createPost(const std::string& community_name, const std::string& title,
                    const std::string& body = "", const std::string& url = "");
    bool likePost(int post_id, int score); // 1=up, 0=neutral, -1=down

    // Comments
    std::vector<LemmyComment> listComments(int post_id, int limit = 20);
    int createComment(int post_id, const std::string& content, int parent_id = 0);

private:
    json get(const std::string& path, const std::map<std::string, std::string>& params = {});
    json post(const std::string& path, const json& body);
    std::string buildUrl(const std::string& path);

    std::string _instance;
    std::string _jwt;
    http::Client _http;
};

}} // namespace matrixcli::lemmy
