#pragma once
#include <string>
#include <cstdint>
#include <vector>

std::string createAccount(const std::string& json);
std::string generateOneTimeKeys(const std::string& json);
std::string signMessage(const std::string& json);
std::string getIdentityKey(const std::string& json);
std::string getFingerprintKey(const std::string& json);
