#ifndef PROGRESSIVE_MARKDOWN_HPP
#define PROGRESSIVE_MARKDOWN_HPP

#include <string>

namespace progressive {

struct MdConfig {
    bool enableTables = true;
    bool enableLinks = true;
    bool enableCodeBlocks = true;
    bool enableImages = false; // matrix-specific: images handled separately
    bool enableHtmlPassthrough = true; // pass through <del>, <u>, <font> (matrix extensions)
    bool enableHorizontalScroll = true; // wrap tables in scrollable container
};

// Render markdown to HTML suitable for Android TextView rendering.
// Supports: headings, bold, italic, strikethrough, code, links, blockquotes,
//           ordered/unordered lists, and tables (with horizontal scroll).
std::string markdownToHtml(const std::string& markdown, const MdConfig& config = MdConfig{});

// Parse a markdown table and return HTML with horizontal scroll wrapper.
// Table format:
// | Header 1 | Header 2 |
// |----------|----------|
// | Cell 1   | Cell 2   |
std::string parseMarkdownTable(const std::string& tableBlock, bool withScroll);

} // namespace progressive

#endif // PROGRESSIVE_MARKDOWN_HPP
