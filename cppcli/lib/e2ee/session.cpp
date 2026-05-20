#include "session.hpp"

namespace matrixcli { namespace e2ee {

json OlmSessionData::toJSON() const {
    return {
        {"session_id", session_id},
        {"identity_key", identity_key},
        {"first_known_index", first_known_index},
        {"message_index", message_index}
    };
}

OlmSessionData OlmSessionData::fromJSON(const json& j) {
    OlmSessionData d;
    d.session_id = j.value("session_id", "");
    d.identity_key = j.value("identity_key", "");
    d.first_known_index = j.value("first_known_index", 0);
    d.message_index = j.value("message_index", 0);
    return d;
}

}} // namespace matrixcli::e2ee
