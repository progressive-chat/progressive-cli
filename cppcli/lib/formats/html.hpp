#pragma once

#include "json.hpp"

namespace matrixcli { namespace formats {

class HTMLRenderer : public FormatRenderer {
public:
    std::string contentType() const override { return "text/html; charset=utf-8"; }
    std::string render(const RenderContext& ctx) const override;
};

}} // namespace matrixcli::formats
