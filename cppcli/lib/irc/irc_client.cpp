#include "irc_client.hpp"
#include "../util/logger.hpp"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace matrixcli { namespace irc {

IrcClient::IrcClient() {}
IrcClient::~IrcClient() { disconnect(); }

void IrcClient::setState(IrcState s) {
    _state = s;
    if (_stateCb) _stateCb(s);
}

void IrcClient::sendRaw(const std::string& line) {
    if (_sock < 0) return;
    std::string data = line + "\r\n";
    send(_sock, data.c_str(), data.size(), 0);
}

bool IrcClient::connect() {
    if (_sock >= 0) disconnect();

    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(_cfg.host.c_str(), std::to_string(_cfg.port).c_str(), &hints, &res) != 0) {
        setState(IrcState::Error);
        return false;
    }

    _sock = socket(res->ai_family, res->ai_socktype, 0);
    if (_sock < 0 || ::connect(_sock, res->ai_addr, res->ai_addrlen) < 0) {
        setState(IrcState::Error);
        close(_sock); _sock = -1;
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    setState(IrcState::Connected);

    // Register
    if (!_cfg.password.empty()) sendRaw("PASS " + _cfg.password);
    sendRaw("NICK " + _cfg.nick);
    sendRaw("USER " + _cfg.user + " 0 * :" + _cfg.realname);
    setState(IrcState::Connecting);

    _running = true;
    _thread = std::make_unique<std::thread>(&IrcClient::eventLoop, this);
    return true;
}

void IrcClient::disconnect() {
    _running = false;
    if (_thread && _thread->joinable()) _thread->join();
    if (_sock >= 0) { close(_sock); _sock = -1; }
    setState(IrcState::Disconnected);
}

void IrcClient::join(const std::string& channel) { sendRaw("JOIN " + channel); }
void IrcClient::part(const std::string& channel, const std::string& reason) {
    sendRaw("PART " + channel + (reason.empty() ? "" : " :" + reason));
}
void IrcClient::privmsg(const std::string& target, const std::string& text) {
    sendRaw("PRIVMSG " + target + " :" + text);
}
void IrcClient::notice(const std::string& target, const std::string& text) {
    sendRaw("NOTICE " + target + " :" + text);
}
void IrcClient::whois(const std::string& nick) { sendRaw("WHOIS " + nick); }
void IrcClient::topic(const std::string& channel) { sendRaw("TOPIC " + channel); }
void IrcClient::names(const std::string& channel) { sendRaw("NAMES " + channel); }

void IrcClient::parseMessage(const std::string& raw, IrcMessage& msg) {
    msg.raw = raw;
    size_t pos = 0;

    // Prefix: :nick!user@host
    if (raw[pos] == ':') {
        auto sp = raw.find(' ', pos);
        msg.prefix = raw.substr(pos + 1, sp - pos - 1);
        pos = sp + 1;
    }

    // Command
    auto sp = raw.find(' ', pos);
    msg.command = raw.substr(pos, sp - pos);
    pos = sp != std::string::npos ? sp + 1 : raw.size();

    // Target (channel or nick)
    if (pos < raw.size() && raw[pos] != ':') {
        auto sp2 = raw.find(' ', pos);
        msg.target = raw.substr(pos, sp2 - pos);
        pos = sp2 != std::string::npos ? sp2 + 1 : raw.size();
    }

    // Body (after :)
    if (pos < raw.size() && raw[pos] == ':') {
        msg.body = raw.substr(pos + 1);
        // CTCP detection
        if (msg.body.size() > 1 && msg.body.front() == '\x01' && msg.body.back() == '\x01') {
            msg.is_ctcp = true;
            msg.body = msg.body.substr(1, msg.body.size() - 2);
        }
    }

    // Convert command to uppercase
    std::transform(msg.command.begin(), msg.command.end(), msg.command.begin(), ::toupper);
}

void IrcClient::handleLine(const std::string& line) {
    IrcMessage msg;
    parseMessage(line, msg);

    // Handle server responses
    if (msg.command == "PING") {
        sendRaw("PONG :" + msg.body);
        return;
    }
    if (msg.command == "001") { // RPL_WELCOME
        setState(IrcState::Registered);
        for (auto& ch : _cfg.autojoin) join(ch);
        return;
    }
    if (msg.command == "433") { // ERR_NICKNAMEINUSE
        _cfg.nick += "_";
        sendRaw("NICK " + _cfg.nick);
        return;
    }
    if (msg.command == "353") { // RPL_NAMREPLY
        // Parse names: = #channel :user1 user2 @op3
        auto chPos = msg.target.find('#'); // skip the '=' or '*' channel type prefix before the actual channel name
        std::string channel;
        size_t bodyPos = 0;
        if (msg.body.find(" :") != std::string::npos) {
            auto colonPos = msg.body.find(" :");
            // target has the channel after the type prefix
            channel = msg.target.substr(msg.target.find('#'));
            auto namesStr = msg.body.substr(colonPos + 2);
            std::istringstream nss(namesStr);
            std::string nick;
            while (nss >> nick) {
                if (!nick.empty() && nick[0] == '@') nick = nick.substr(1);
                if (!nick.empty() && nick[0] == '+') nick = nick.substr(1);
                _channels[channel].push_back(nick);
            }
        }
        return;
    }
    if (msg.command == "JOIN") {
        _channels[msg.body.empty() ? msg.target : msg.body].push_back(msg.prefix.substr(0, msg.prefix.find('!')));
    }
    if (msg.command == "PART") {
        auto& users = _channels[msg.target];
        std::string nick = msg.prefix.substr(0, msg.prefix.find('!'));
        users.erase(std::remove(users.begin(), users.end(), nick), users.end());
    }

    if (_msgCb) _msgCb(msg);
}

void IrcClient::eventLoop() {
    char buf[4096];
    std::string buffer;
    while (_running.load() && _sock >= 0) {
        ssize_t n = recv(_sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { setState(IrcState::Disconnected); break; }
        buf[n] = '\0';
        buffer += buf;

        size_t pos;
        while ((pos = buffer.find("\r\n")) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            if (!line.empty()) handleLine(line);
        }
        if (buffer.find('\n') != std::string::npos) {
            size_t p2 = buffer.find('\n');
            handleLine(buffer.substr(0, p2));
            buffer.erase(0, p2 + 1);
        }
    }
}

}} // namespace matrixcli::irc
