// The unit tests for the db settings logic (the receipts policy and the
// last-read marker) and the invite queries (the state_key fix).
#include <cassert>
#include <cstdio>
#include <string>

#include "../lib/database/db.hpp"

using matrixcli::db::Database;
using matrixcli::matrix::Event;
using nlohmann::json;

int main() {
    const std::string path = "/tmp/matrixcli_test_db.sqlite";
    std::remove(path.c_str());
    {
        Database db;
        assert(db.open(path));

        // The receipts policy: on by default, off persists, the list works.
        assert(db.receiptsEnabled("!a:hs"));
        db.setReceiptsEnabled("!a:hs", false);
        assert(!db.receiptsEnabled("!a:hs"));
        assert(db.receiptsOffRooms().size() == 1);
        assert(db.receiptsOffRooms()[0] == "!a:hs");
        db.setReceiptsEnabled("!a:hs", true);
        assert(db.receiptsEnabled("!a:hs"));
        assert(db.receiptsOffRooms().empty());

        // The last-read marker.
        assert(db.getReadMarker("!a:hs").empty());
        db.setReadMarker("!a:hs", "$event1");
        assert(db.getReadMarker("!a:hs") == "$event1");

        // The invites: an invite member event FOR the user (the state_key
        // holds the invitee — the query used to match the sender).
        Event invite;
        invite.event_id = "$inv1";
        invite.room_id = "!design:hs";
        invite.sender = "@alice:hs";
        invite.type = "m.room.member";
        invite.state_key = "@you:hs";
        invite.origin_server_ts = 1000;
        invite.content = {{"membership", "invite"},
                          {"reason", "join us"}};
        db.upsertRoom(json{{"name", "design"}}, "!design:hs");
        db.insertEvent(invite);

        auto open = db.openInvites("@you:hs");
        assert(open.size() == 1);
        assert(open[0].roomId == "!design:hs");
        assert(open[0].inviter == "@alice:hs");
        assert(open[0].reason == "join us");
        assert(open[0].ts == 1000);
        assert(db.inviteCount("@you:hs") == 1);
        assert(db.invitedRoomIds("@you:hs").size() == 1);

        // A later join closes the invite.
        Event join = invite;
        join.event_id = "$join1";
        join.content = {{"membership", "join"}};
        join.origin_server_ts = 2000;
        db.insertEvent(join);
        assert(db.openInvites("@you:hs").empty());
        assert(db.inviteCount("@you:hs") == 0);

        // A different user's localpart must not match ("you" vs "youth").
        db.setReadMarker("!x:hs", "$e");
        Event otherInvite = invite;
        otherInvite.event_id = "$inv2";
        otherInvite.state_key = "@youth:hs";
        db.insertEvent(otherInvite);
        assert(db.inviteCount("@you:hs") == 0);
    }
    std::remove(path.c_str());
    std::printf("test_db_settings: all ok\n");
    return 0;
}
