#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "../api/server.hpp"

namespace matrixcli { namespace formats {

struct RenderContext {
    nlohmann::json data;
    api::Format format = api::Format::JSON;
};

class FormatRenderer {
public:
    virtual ~FormatRenderer() = default;
    virtual std::string contentType() const = 0;
    virtual std::string render(const RenderContext& ctx) const = 0;

    static std::unique_ptr<FormatRenderer> create(api::Format format);
};

}} // namespace matrixcli::formats
