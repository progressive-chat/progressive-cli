#include "progressive/markdown.hpp"
#include <sstream>
#include <regex>
#include <vector>

namespace progressive {

static std::string escapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:  out += c;
        }
    }
    return out;
}

// Process inline formatting: **bold**, *italic*, ~~strike~~, `code`, [link](url)
static std::string processInline(const std::string& line, const MdConfig& config) {
    std::string result = line;

    // Code spans: `code`
    {
        std::regex codeRe(R"(`([^`]+)`)");
        result = std::regex_replace(result, codeRe, "<code>$1</code>");
    }

    // Bold: **text** or __text__
    {
        std::regex boldRe(R"(\*\*([^*]+)\*\*)");
        result = std::regex_replace(result, boldRe, "<b>$1</b>");
    }
    {
        std::regex boldRe2(R"(__([^_]+)__)");
        result = std::regex_replace(result, boldRe2, "<b>$1</b>");
    }

    // Italic: *text* or _text_
    {
        std::regex italicRe(R"(\*([^*]+)\*)");
        result = std::regex_replace(result, italicRe, "<i>$1</i>");
    }
    {
        std::regex italicRe2(R"(_([^_]+)_)");
        result = std::regex_replace(result, italicRe2, "<i>$1</i>");
    }

    // Strikethrough: ~~text~~
    {
        std::regex strikeRe(R"(~~([^~]+)~~)");
        result = std::regex_replace(result, strikeRe, "<s>$1</s>");
    }

    // Links: [text](url)
    if (config.enableLinks) {
        std::regex linkRe(R"(\[([^\]]+)\]\(([^)]+)\))");
        result = std::regex_replace(result, linkRe, R"(<a href="$2">$1</a>)");
    }

    return result;
}

static std::string parseTable(const std::string& tableBlock, bool withScroll) {
    std::istringstream stream(tableBlock);
    std::string line;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
    bool inHeader = true;
    bool inSeparator = false;

    while (std::getline(stream, line)) {
        // Skip empty lines
        if (line.empty()) continue;
        // Must start with |
        if (line[0] != '|') break;

        // Split by |
        std::vector<std::string> cells;
        size_t start = 1;
        while (start < line.size()) {
            size_t end = line.find('|', start);
            if (end == std::string::npos) {
                std::string cell = line.substr(start);
                while (!cell.empty() && cell.back() == ' ') cell.pop_back();
                while (!cell.empty() && cell.front() == ' ') cell.erase(0, 1);
                cells.push_back(cell);
                break;
            }
            std::string cell = line.substr(start, end - start);
            while (!cell.empty() && cell.back() == ' ') cell.pop_back();
            while (!cell.empty() && cell.front() == ' ') cell.erase(0, 1);
            cells.push_back(cell);
            start = end + 1;
        }

        if (inHeader) {
            headers = std::move(cells);
            inHeader = false;
            inSeparator = true;
        } else if (inSeparator) {
            // Check if this is the separator row (---|---)
            bool isSep = true;
            for (const auto& cell : cells) {
                for (char c : cell) {
                    if (c != '-' && c != ':' && c != ' ') {
                        isSep = false;
                        break;
                    }
                }
                if (!isSep) break;
            }
            if (isSep) {
                inSeparator = false;
            } else {
                // Not a separator — treat as data row
                rows.push_back(std::move(cells));
                inSeparator = false;
            }
        } else {
            rows.push_back(std::move(cells));
        }
    }

    if (headers.empty()) return {};

    std::ostringstream html;

    // Horizontal scroll wrapper
    if (withScroll) {
        html << "<div style=\"overflow-x:auto; display:block; white-space:nowrap;\">";
    }

    html << "<table style=\"border-collapse:collapse; width:100%; margin:8px 0;\">";

    // Header row
    html << "<thead><tr>";
    for (const auto& h : headers) {
        html << "<th style=\"border:1px solid #ccc; padding:6px 12px; background:#f5f5f5; font-weight:600;\">"
             << escapeHtml(h) << "</th>";
    }
    html << "</tr></thead>";

    // Data rows
    html << "<tbody>";
    for (const auto& row : rows) {
        html << "<tr>";
        size_t col = 0;
        for (const auto& cell : row) {
            html << "<td style=\"border:1px solid #ccc; padding:6px 12px;\">"
                 << processInline(cell, MdConfig{}) << "</td>";
            ++col;
        }
        // Pad missing columns
        while (col < headers.size()) {
            html << "<td></td>";
            ++col;
        }
        html << "</tr>";
    }
    html << "</tbody>";
    html << "</table>";

    if (withScroll) {
        html << "</div>";
    }

    return html.str();
}

std::string markdownToHtml(const std::string& markdown, const MdConfig& config) {
    std::istringstream stream(markdown);
    std::ostringstream html;
    std::string line;
    bool inCodeBlock = false;
    std::string codeBlockContent;
    std::string codeBlockLang;
    bool inBlockquote = false;
    bool inList = false;
    bool inOrderedList = false;
    bool inTable = false;
    std::string tableBlock;
    int listCount = 0;
    std::string prevLine;

    auto flushParagraph = [&]() {
        if (inList) { html << "</ul>\n"; inList = false; }
        if (inOrderedList) { html << "</ol>\n"; inOrderedList = false; }
    };

    while (std::getline(stream, line) || !codeBlockContent.empty()) {
        bool reachedEnd = stream.eof();

        // Code block handling
        if (config.enableCodeBlocks && (line.rfind("```", 0) == 0)) {
            if (!inCodeBlock) {
                flushParagraph();
                // Opening fence
                codeBlockLang = line.substr(3);
                while (!codeBlockLang.empty() && codeBlockLang[0] == ' ') codeBlockLang.erase(0, 1);
                inCodeBlock = true;
                codeBlockContent.clear();
                continue;
            } else {
                // Closing fence
                html << "<pre><code";
                if (!codeBlockLang.empty()) html << " class=\"language-" << escapeHtml(codeBlockLang) << "\"";
                html << ">";
                html << escapeHtml(codeBlockContent);
                html << "</code></pre>\n";
                inCodeBlock = false;
                codeBlockContent.clear();
                codeBlockLang.clear();
                continue;
            }
        }

        if (inCodeBlock) {
            if (!codeBlockContent.empty()) codeBlockContent += "\n";
            codeBlockContent += line;
            continue;
        }

        // Table detection
        if (config.enableTables && !line.empty() && line[0] == '|' && line.find('|', 1) != std::string::npos) {
            if (!inTable) {
                flushParagraph();
                inTable = true;
                tableBlock.clear();
            }
            tableBlock += line + "\n";
            // If next line is not a table row, close the table
            auto nextPos = stream.tellg();
            std::string nextLine;
            bool hasNext = (bool)std::getline(stream, nextLine);
            if (hasNext) stream.seekg(nextPos);
            if (!hasNext || nextLine.empty() || nextLine[0] != '|') {
                if (inTable) {
                    html << parseTable(tableBlock, config.enableHorizontalScroll) << "\n";
                    inTable = false;
                    tableBlock.clear();
                }
            }
            continue;
        }

        // Heading: # ## ### etc.
        if (line.rfind("#", 0) == 0) {
            flushParagraph();
            int level = 1;
            while (level < (int)line.size() && line[level] == '#') ++level;
            if (level <= 6 && level < (int)line.size() && line[level] == ' ') {
                auto heading = line.substr(level + 1);
                html << "<h" << level << ">" << processInline(heading, config) << "</h" << level << ">\n";
                continue;
            }
        }

        // Blockquote
        if (line.rfind("> ", 0) == 0) {
            if (!inBlockquote) {
                flushParagraph();
                html << "<blockquote>";
                inBlockquote = true;
            }
            html << "<p>" << processInline(line.substr(2), config) << "</p>\n";
            continue;
        } else if (inBlockquote) {
            html << "</blockquote>\n";
            inBlockquote = false;
        }

        // Unordered list
        if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
            if (inOrderedList) { html << "</ol>\n"; inOrderedList = false; }
            if (!inList) { html << "<ul>\n"; inList = true; }
            html << "<li>" << processInline(line.substr(2), config) << "</li>\n";
            continue;
        }

        // Ordered list
        std::regex orderedRe(R"(^(\d+)\.\s)");
        std::smatch omatch;
        if (std::regex_search(line, omatch, orderedRe)) {
            if (inList) { html << "</ul>\n"; inList = false; }
            if (!inOrderedList) { html << "<ol>\n"; inOrderedList = true; }
            auto content = line.substr(omatch.length());
            html << "<li>" << processInline(content, config) << "</li>\n";
            continue;
        }

        // Horizontal rule
        if (line == "---" || line == "***" || line == "___") {
            flushParagraph();
            html << "<hr>\n";
            continue;
        }

        // Empty line: paragraph break
        if (line.empty()) {
            flushParagraph();
            if (!prevLine.empty() && !inBlockquote) {
                html << "<br>\n";
            }
            prevLine = line;
            continue;
        }

        // Regular paragraph text
        if (!inList && !inOrderedList) {
            html << "<p>" << processInline(line, config) << "</p>\n";
        }

        prevLine = line;
    }

    flushParagraph();
    if (inBlockquote) html << "</blockquote>\n";

    return html.str();
}

std::string parseMarkdownTable(const std::string& tableBlock, bool withScroll) {
    return parseTable(tableBlock, withScroll);
}

} // namespace progressive
