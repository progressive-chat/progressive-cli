#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>

namespace matrixcli { namespace tui {

class Screen;

class MainView {
public:
    MainView();
    ~MainView();

    void addMessage(const std::string& sender, const std::string& body);
    void setStatus(const std::string& status);
    void run(Screen& screen);
    void stop();

private:
    void render(Screen& screen, int scroll_offset);

    std::vector<std::pair<std::string, std::string>> _messages;
    std::string _status;
    std::string _input_buffer;
    std::atomic<bool> _running{false};
    int _scroll_offset = 0;
};

}} // namespace matrixcli::tui
