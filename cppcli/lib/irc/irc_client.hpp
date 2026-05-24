#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <map>

namespace matrixcli { namespace irc {

enum class IrcState { Disconnected, Connecting, Connected, Registered, Error };

struct IrcServerConfig {
    std::string host = "irc.libera.chat";
    int port = 6667;
    bool tls = false;
    std::string nick = "matrixcli";
    std::string user = "matrixcli";
    std::string realname = "matrixcli IRC client";
    std::string password; // optional server password
    std::vector<std::string> autojoin;
};

struct IrcMessage {
    std::string prefix;  // :nick!user@host
    std::string command; // PRIVMSG, JOIN, etc.
    std::string target;  // channel or nickname
    std::string body;    // message text
    bool is_ctcp = false;
    std::string raw;
};

class IrcClient {
public:
    IrcClient();
    ~IrcClient();

    using MessageCallback = std::function<void(const IrcMessage&)>;
    using StateCallback = std::function<void(IrcState)>;

    void setConfig(const IrcServerConfig& cfg) { _cfg = cfg; }
    void onMessage(MessageCallback cb) { _msgCb = std::move(cb); }
    void onStateChange(StateCallback cb) { _stateCb = std::move(cb); }

    bool connect();
    void disconnect();
    IrcState state() const { return _state.load(); }

    // Commands
    void join(const std::string& channel);
    void part(const std::string& channel, const std::string& reason = "");
    void privmsg(const std::string& target, const std::string& text);
    void notice(const std::string& target, const std::string& text);
    void whois(const std::string& nick);
    void topic(const std::string& channel);
    void names(const std::string& channel);

private:
    void sendRaw(const std::string& line);
    void eventLoop();
    void handleLine(const std::string& line);
    void parseMessage(const std::string& raw, IrcMessage& msg);
    void setState(IrcState s);

    IrcServerConfig _cfg;
    std::atomic<IrcState> _state{IrcState::Disconnected};
    int _sock = -1;
    std::unique_ptr<std::thread> _thread;
    std::atomic<bool> _running{false};
    MessageCallback _msgCb;
    StateCallback _stateCb;
    std::map<std::string, std::vector<std::string>> _channels; // channel -> users
};

}} // namespace matrixcli::irc
