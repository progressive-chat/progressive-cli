#pragma once

#include "server.hpp"

namespace matrixcli { namespace api {

inline Format detectFormat(const std::string& accept, const std::string& queryParam) {
    if (!queryParam.empty()) {
        if (queryParam == "json")     return Format::JSON;
        if (queryParam == "text")     return Format::Text;
        if (queryParam == "markdown") return Format::Markdown;
        if (queryParam == "gemini")   return Format::Gemini;
        if (queryParam == "html")     return Format::HTML;
    }

    if (accept.find("text/markdown") != std::string::npos) return Format::Markdown;
    if (accept.find("text/gemini") != std::string::npos)   return Format::Gemini;
    if (accept.find("text/html") != std::string::npos)     return Format::HTML;
    if (accept.find("text/plain") != std::string::npos)    return Format::Text;
    if (accept.find("application/json") != std::string::npos) return Format::JSON;

    return Format::JSON;
}

}} // namespace matrixcli::api
