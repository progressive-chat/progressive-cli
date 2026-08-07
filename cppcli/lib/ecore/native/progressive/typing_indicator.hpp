#pragma once
#include <string>
#include <cstdint>

std::string parseTypingEvent(const std::string& json);
std::string isTyping(const std::string& json);
std::string getTypingTimeout(const std::string& json);
std::string formatTypingList(const std::string& json);
std::string buildTypingNotification(const std::string& json);
