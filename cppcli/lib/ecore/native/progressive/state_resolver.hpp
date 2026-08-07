#pragma once
#include <string>
#include <cstdint>
#include <vector>

std::string resolveStateConflict(const std::string& json);
std::string mergeState(const std::string& json);
std::string getAuthChain(const std::string& json);
std::string validateStateEvent(const std::string& json);
std::string applyStateResolution(const std::string& json);
