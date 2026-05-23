#pragma once

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <atomic>

#include "../http/http.hpp"
#include "auth.hpp"
#include "events.hpp"
#include "error.hpp"
#include "pushrules.hpp"

namespace matrixcli { namespace db { class Database; } }
namespace matrixcli { namespace e2ee { class CryptoManager; } }

namespace matrixcli { namespace matrix {

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // Connection settings
    void setHomeserverURL(const std::string& url);
    std::string homeserverURL() const;
    void setProxy(const http::ProxyConfig& config);
    void setTimeout(int seconds);

    // Access token
    void setAccessToken(const std::string& token);

    // Database (optional persistent storage)
    void setDatabase(db::Database* db);

    // Crypto
    bool initCrypto(const std::string& userId, const std::string& deviceId);
    bool enableEncryption(const std::string& roomId);
    bool isRoomEncrypted(const std::string& roomId) const;

    // Cross-signing
    bool loadCrossSigningKeys();
    bool uploadDeviceKeys();

    // Key backup
    bool uploadKeyBackup();
    bool restoreKeyBackup();

    // Server discovery
    ServerVersions getServerVersions();
    WellKnown getWellKnown(const std::string& server_name);

    // Auth
    LoginFlowsResult getLoginFlows();
    Credentials loginPassword(const std::string& username,
                              const std::string& password,
                              const std::string& device_name = "");
    Credentials loginToken(const std::string& token,
                           const std::string& device_name = "");
    Credentials loginSSO(const std::string& token,
                          const std::string& device_name = "");
    Credentials registerAccount(const std::string& username,
                                 const std::string& password,
                                 const std::string& device_name = "");
    SessionInfo whoAmI();
    bool logout();
    bool logoutAll();

    // Sync
    using EventCallback = std::function<void(const Event&)>;
    void startSync(EventCallback onEvent, const std::string& filter = "",
                   int poll_timeout_ms = 30000);
    void stopSync();
    SyncResponse syncOnce(const std::string& filter = "",
                          const std::string& since = "",
                          int timeout_ms = 30000);
    std::string nextBatch() const;

    // Messages
    std::string sendMessage(const std::string& room_id,
                            const std::string& body,
                            const std::string& msgtype = "m.text");
    std::string sendTextMessage(const std::string& room_id,
                                 const std::string& body);
    std::string sendThreadReply(const std::string& room_id,
                                 const std::string& thread_root,
                                 const std::string& body);
    std::string sendNotice(const std::string& room_id,
                           const std::string& body);
    std::string sendEmote(const std::string& room_id,
                          const std::string& body);
    std::string sendImageMessage(const std::string& room_id,
                                 const std::string& mxc_url,
                                 const std::string& filename,
                                 int64_t size,
                                 const std::string& mimetype,
                                 int width = 0, int height = 0);
    std::string sendFileMessage(const std::string& room_id,
                                const std::string& mxc_url,
                                const std::string& filename,
                                int64_t size,
                                const std::string& mimetype);
    std::string sendEvent(const std::string& room_id,
                          const std::string& event_type,
                          const json& content);
    std::string sendStateEvent(const std::string& room_id,
                               const std::string& event_type,
                               const std::string& state_key,
                               const json& content);
    std::string redactEvent(const std::string& room_id,
                            const std::string& event_id,
                            const std::string& reason = "");

    // Room operations
    std::string createRoom(const std::string& name = "",
                           const std::string& topic = "",
                           bool is_direct = false,
                           const std::vector<std::string>& invite_users = {});
    bool joinRoom(const std::string& room_id, const std::string& reason = "");
    bool leaveRoom(const std::string& room_id);
    bool inviteUser(const std::string& room_id, const std::string& user_id,
                    const std::string& reason = "");
    bool kickUser(const std::string& room_id, const std::string& user_id,
                  const std::string& reason = "");
    bool banUser(const std::string& room_id, const std::string& user_id,
                 const std::string& reason = "");
    bool unbanUser(const std::string& room_id, const std::string& user_id);
    bool ignoreUser(const std::string& user_id);

    // Power levels
    json getPowerLevels(const std::string& room_id);
    bool setPowerLevel(const std::string& room_id, const std::string& user_id, int level);

    // Room tags
    bool setRoomTag(const std::string& room_id, const std::string& tag, float order = 0.5);
    bool deleteRoomTag(const std::string& room_id, const std::string& tag);
    json getRoomTags(const std::string& room_id);

    // Pinned events
    json getPinnedEvents(const std::string& room_id);
    bool pinEvent(const std::string& room_id, const std::string& event_id);
    bool unpinEvent(const std::string& room_id, const std::string& event_id);

    // Room upgrade
    std::string upgradeRoom(const std::string& room_id, const std::string& new_version = "10");

    // Room mirror
    bool mirrorMessage(const std::string& from_room, const std::string& event_id,
                       const std::string& to_room);
    bool mirrorEvent(const std::string& from_room, const std::string& event_id,
                     const std::string& to_room);

    // Room analytics
    json getRoomStats(const std::string& room_id);

    // Room state
    std::string setRoomName(const std::string& room_id, const std::string& name);
    std::string setRoomTopic(const std::string& room_id, const std::string& topic);
    bool setRoomAvatar(const std::string& room_id, const std::string& mxc_url);
    std::vector<Event> getRoomState(const std::string& room_id);
    std::vector<Event> getRoomMembers(const std::string& room_id);
    std::vector<Event> getRoomMessages(const std::string& room_id,
                                       const std::string& from = "",
                                       const std::string& dir = "b",
                                       int limit = 20);

    // Profile
    UserInfo getProfile(const std::string& user_id);
    std::string getDisplayName(const std::string& user_id);
    bool setDisplayName(const std::string& display_name);
    std::string getAvatarUrl(const std::string& user_id);
    bool setAvatarUrl(const std::string& avatar_url);

    // Devices
    json getDevices();
    bool deleteDevices(const std::vector<std::string>& device_ids);

    // Push rules
    json getPushRules();
    void loadPushRules();
    PushResult evaluatePush(const json& event);

    // DM tracking
    void loadDirectChats();
    bool isDirectChat(const std::string& room_id) const;
    std::string dmUserId(const std::string& room_id) const;

    // Space hierarchy
    std::vector<std::string> getSpaceChildren(const std::string& space_id) const;
    bool isSpaceRoom(const std::string& room_id) const;

    // SSO login
    std::string getSSOLoginURL(const std::string& redirect_uri = "");

    // Typing notification
    bool sendTyping(const std::string& room_id, bool typing, int timeout_ms = 30000);

    // Polls
    std::string sendPollResponse(const std::string& room_id, const std::string& poll_event_id,
                                  const std::vector<std::string>& answer_ids);

    // Reactions
    std::string sendReaction(const std::string& room_id, const std::string& event_id,
                              const std::string& key);

    // Special message types
    std::string sendVoiceMessage(const std::string& room_id, const std::string& mxc_url,
                                  int64_t duration_ms, const std::string& mimetype = "audio/ogg");
    std::string sendSticker(const std::string& room_id, const std::string& mxc_url,
                             const std::string& body, const std::string& mimetype = "image/webp");
    std::string sendLocation(const std::string& room_id, const std::string& geo_uri,
                              const std::string& description = "");
    std::string sendTodo(const std::string& room_id, const std::string& title,
                          const json& items);

    // URL preview
    json getURLPreview(const std::string& url);

    // Filters
    std::string createFilter(const std::string& filter_json);

    // Public rooms
    json getPublicRooms(const std::string& server = "",
                        const std::string& search_term = "",
                        int limit = 20);

    // Search
    json searchMessages(const std::string& search_term,
                        const std::string& room_id = "",
                        int limit = 10);

    // Account
    bool changePassword(const std::string& old_password,
                        const std::string& new_password);
    bool deactivateAccount(const std::string& auth_json = "{}");

    // Presence
    bool setPresence(const std::string& presence);

    // Read receipts
    bool sendReadReceipt(const std::string& room_id, const std::string& event_id);
    json getPresence(const std::string& user_id);

    // Notifications
    json getNotifications(const std::string& from = "",
                          int limit = 20,
                          const std::string& only = "");

    // Media
    std::string uploadMedia(const std::string& file_path,
                            const std::string& content_type = "");

    // Utility
    bool isLoggedIn() const;
    std::string userId() const;
    Credentials credentials() const;
    std::string generateTxnId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    std::string buildUrl(const std::string& path) const;
    std::string buildUrl(const std::string& path,
                         const std::map<std::string, std::string>& params) const;

    http::Response authGet(const std::string& path, int timeout = 30000);
    http::Response authGet(const std::string& path,
                           const std::map<std::string, std::string>& params,
                           int timeout = 30000);

    http::Response authPost(const std::string& path,
                            const std::string& json_body,
                            int timeout = 30000);

    http::Response authPut(const std::string& path,
                           const std::string& json_body,
                           int timeout = 30000);

    http::Response authDelete(const std::string& path, int timeout = 30000);

    void checkResponse(const http::Response& resp);
    MatrixError makeMatrixError(const http::Response& resp);
};

}} // namespace matrixcli::matrix
