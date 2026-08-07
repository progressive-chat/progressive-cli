#ifndef PROGRESSIVE_STRING_UTILS_HPP
#define PROGRESSIVE_STRING_UTILS_HPP

#include <string>
#include <vector>

namespace progressive {

// Remove characters that are invalid in Matrix room names.
// Keeps: letters, digits, spaces, common punctuation.
// Replaces: multiple spaces with single space, trims.
std::string sanitizeRoomName(const std::string& input);

// Replace various space characters (nbsp, thin space, etc.) with regular space.
std::string replaceSpaceChars(const std::string& input);

// Split a string by delimiter.
std::vector<std::string> splitString(const std::string& input, char delimiter);

// Join strings with delimiter.
std::string joinStrings(const std::vector<std::string>& parts, const std::string& delimiter);

// Trim whitespace from both ends.
std::string trim(const std::string& input);

// Check if string starts with prefix.
bool startsWith(const std::string& s, const std::string& prefix);

// Check if string ends with suffix.
bool endsWith(const std::string& s, const std::string& suffix);

// Convert to lowercase.
std::string toLower(const std::string& input);

// Convert to uppercase.
std::string toUpper(const std::string& input);

// Replace all occurrences of `from` with `to`.
std::string replaceAll(const std::string& input, const std::string& from, const std::string& to);

// Check if a string contains only digits.
bool isDigitsOnly(const std::string& input);

// Check if a string contains only letters.
bool isLettersOnly(const std::string& input);

// Check if a string is empty or whitespace only.
bool isBlank(const std::string& input);

// Remove HTML tags from a string.
std::string stripHtmlTags(const std::string& input);

// Count words in a string.
int wordCount(const std::string& input);

// Estimate reading time in seconds (200 WPM average).
int estimateReadingTimeSeconds(const std::string& input);

// Extract the first N words from a string.
std::string firstNWords(const std::string& input, int n);

// Format a count to short decimal: 42 → "42", 1234 → "1.2k", 12345 → "12k", 1.5M → "1.5M"
// Faithful port from im.vector.app.core.utils.TextUtils.kt (147L): formatCountToShortDecimal
// Original: TreeMap of {1000:"k", 1000000:"M", 1000000000:"G"} → floorEntry
std::string formatCountToShortDecimal(int64_t value);


enum class StringDistance {
    LEVENSHTEIN,
    DAMERAU_LEVENSHTEIN,
    HAMMING,
    JARO_WINKLER
};

// Original Kotlin: computeStringDistance — raw edit distance
int computeStringDistance(const std::string& a, const std::string& b, StringDistance algorithm = StringDistance::LEVENSHTEIN);

// Original Kotlin: computeStringSimilarity — 0.0 to 1.0 similarity score
double computeStringSimilarity(const std::string& a, const std::string& b, StringDistance algorithm = StringDistance::LEVENSHTEIN);

// Original Kotlin: findClosestMatch — find best match in candidate list
std::string findClosestMatch(const std::string& target, const std::vector<std::string>& candidates, StringDistance algorithm = StringDistance::LEVENSHTEIN);

// Original Kotlin: StringTruncation mode
enum class StringTruncation {
    END,     // "Hello..."  — truncate from end
    MIDDLE,  // "Hel...ld"  — truncate from middle
    START    // "...World"  — truncate from start
};

// Original Kotlin: truncateString with mode
std::string truncateString(const std::string& input, int maxLen, StringTruncation mode = StringTruncation::END, const std::string& ellipsis = "...");

// Original Kotlin: ellipsizeString — ensure string fits with "..."
std::string ellipsizeString(const std::string& input, int maxLen);

// Original Kotlin: capitalizeFirst — uppercase first character
std::string capitalizeFirst(const std::string& input);

// Original Kotlin: titleCase — capitalize first letter of each word
std::string titleCase(const std::string& input);

// Original Kotlin: camelCase — "hello world" -> "helloWorld"
std::string camelCase(const std::string& input);

// Original Kotlin: snakeCase — "helloWorld" -> "hello_world"
std::string snakeCase(const std::string& input);

// Original Kotlin: kebabCase — "helloWorld" -> "hello-world"
std::string kebabCase(const std::string& input);

// Original Kotlin: slugify — URL-friendly slug
std::string slugify(const std::string& input);

// Original Kotlin: isPalindrome — check if string reads same forwards/backwards
bool isPalindrome(const std::string& input);

// Original Kotlin: countWords — count space-delimited words
int countWords(const std::string& input);

// Original Kotlin: countChars — character count (excluding whitespace if specified)
int countChars(const std::string& input, bool ignoreWhitespace = false);

// Original Kotlin: countLines — count newline-separated lines
int countLines(const std::string& input);

// Original Kotlin: splitLines — split string by newlines
std::vector<std::string> splitLines(const std::string& input);

// Original Kotlin: wordWrap — wrap text to given column width
std::string wordWrap(const std::string& input, int lineWidth);

// Original Kotlin: stripAccents — remove diacritics from Latin characters
std::string stripAccents(const std::string& input);

// Original Kotlin: toHexString — convert bytes to hex
std::string toHexString(const std::vector<uint8_t>& bytes);

// Original Kotlin: fromHexString — convert hex string to bytes
std::vector<uint8_t> fromHexString(const std::string& hex);

// Original Kotlin: base64Encode — simple base64 encoding
std::string base64Encode(const std::string& input);

// Original Kotlin: StringSearchResult — occurrence match
struct StringSearchResult {
    bool found = false;
    int position = -1;
    int length = 0;
};

// Original Kotlin: findAllOccurrences — find all non-overlapping matches
std::vector<StringSearchResult> findAllOccurrences(const std::string& haystack, const std::string& needle);

// Original Kotlin: trimLeft — remove leading whitespace
std::string trimLeft(const std::string& input);

// Original Kotlin: trimRight — remove trailing whitespace
std::string trimRight(const std::string& input);

// Original Kotlin: escapeJson — escape a string for JSON embedding
std::string escapeJson(const std::string& input);

// Original Kotlin: unescapeJson — unescape JSON string
std::string unescapeJson(const std::string& input);

// Original Kotlin: urlEncode — percent-encode a string
std::string urlEncode(const std::string& input);

// Original Kotlin: urlDecode — percent-decode a string
std::string urlDecode(const std::string& input);

// Original Kotlin: naturalCompare — case-insensitive natural sort comparison
int naturalCompare(const std::string& a, const std::string& b);

// Original Kotlin: randomString — generate random alphanumeric string
std::string randomString(int length);

// Original Kotlin: md5Hash — simple hash as hex string
std::string md5Hash(const std::string& input);

// Original Kotlin: isAnagram — check if two strings are anagrams
bool isAnagram(const std::string& a, const std::string& b);

// Original Kotlin: reverseString — reverse character order
std::string reverseString(const std::string& input);

// Original Kotlin: repeatString — repeat string n times
std::string repeatString(const std::string& input, int n);

// Original Kotlin: padLeft — left-pad string to width
std::string padLeft(const std::string& input, int width, char padChar = ' ');

// Original Kotlin: padRight — right-pad string to width
std::string padRight(const std::string& input, int width, char padChar = ' ');

// IDN (Internationalized Domain Names) support
// Converts Unicode domain (e.g. "пример.рф") to Punycode (e.g. "xn--e1afmkfd.xn--p1ai")
// Used for DNS resolution and Matrix server names containing non-ASCII characters
std::string toPunycode(const std::string& unicodeDomain);

// Converts Punycode domain back to Unicode for display
// "xn--e1afmkfd.xn--p1ai" → "пример.рф"
std::string fromPunycode(const std::string& punycodeDomain);

// URL utilities (ported from UrlUtils.kt)
std::string ensureProtocol(const std::string& url);
std::string ensureTrailingSlash(const std::string& url);
std::string stripHtmlTags(const std::string& html);

} // namespace progressive

#endif // PROGRESSIVE_STRING_UTILS_HPP
