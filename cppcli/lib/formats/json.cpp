#include "json.hpp"
#include "text.hpp"
#include "markdown.hpp"
#include "gemini.hpp"
#include "html.hpp"

namespace matrixcli { namespace formats {

class JSONRenderer : public FormatRenderer {
public:
    std::string contentType() const override { return "application/json"; }
    std::string render(const RenderContext& ctx) const override {
        return ctx.data.dump(2);
    }
};

std::unique_ptr<FormatRenderer> FormatRenderer::create(api::Format format) {
    switch (format) {
        case api::Format::JSON:     return std::make_unique<JSONRenderer>();
        case api::Format::Text:     return std::make_unique<TextRenderer>();
        case api::Format::Markdown: return std::make_unique<MarkdownRenderer>();
        case api::Format::Gemini:   return std::make_unique<GeminiRenderer>();
        case api::Format::HTML:     return std::make_unique<HTMLRenderer>();
    }
    return std::make_unique<JSONRenderer>();
}

}} // namespace matrixcli::formats
