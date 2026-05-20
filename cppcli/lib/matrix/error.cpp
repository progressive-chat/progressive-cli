#include "error.hpp"
#include <nlohmann/json.hpp>

namespace matrixcli { namespace matrix {

using json = nlohmann::json;

MatrixError parseMatrixError(const std::string& response_body, int http_status) {
    MatrixError err;
    err.http_status = http_status;

    if (response_body.empty()) {
        err.error = "Empty response body";
        return err;
    }

    try {
        auto j = json::parse(response_body);
        err.errcode = j.value("errcode", "");
        err.error = j.value("error", "");
        if (j.contains("retry_after_ms")) {
            err.retry_after_ms = j["retry_after_ms"].get<int>();
        }
    } catch (...) {
        err.error = "Failed to parse error response";
    }

    return err;
}

}} // namespace matrixcli::matrix
