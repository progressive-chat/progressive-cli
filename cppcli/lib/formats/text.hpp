#pragma once

#include "json.hpp"

namespace matrixcli { namespace formats {

class TextRenderer : public FormatRenderer {
public:
    std::string contentType() const override { return "text/plain; charset=utf-8"; }
    std::string render(const RenderContext& ctx) const override;
};

}} // namespace matrixcli::formats
