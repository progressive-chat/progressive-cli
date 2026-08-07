#pragma once

#include <string>

namespace progressive {

// ==== Canonical JSON ====
//
// Implements Matrix Canonical JSON specification:
//   https://matrix.org/docs/spec/appendices#canonical-json
//
// Original Kotlin (JsonCanonicalizer.kt:35-95):
//   object JsonCanonicalizer {
//       fun canonicalize(jsonString: String): String
//       private fun canonicalizeRecursive(any: Any): String
//   }
//
// Rules:
//   1. No whitespace
//   2. Object keys sorted lexicographically (UTF-8 byte order)
//   3. Strings UTF-8 encoded, escaped per JSON spec
//   4. Forward slashes NOT escaped (replace "\\/" → "/")
//
// JSON format: {"key1":"value1","key2":"value2"}

// Canonicalize a JSON string per Matrix spec.
// Handles objects, arrays, strings, numbers, booleans, null.
std::string canonicalizeJson(const std::string& json);

} // namespace progressive
