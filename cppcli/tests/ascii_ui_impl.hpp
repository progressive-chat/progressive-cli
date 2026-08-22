// ascii_ui_impl.hpp — the internal renderer/REPL helpers, shared by the
// split ascii_ui translation units. Not a public header.
#pragma once

#include "ascii_state.hpp"
#include "../lib/database/db.hpp"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace matrixcli {

// The per-codepoint width overrides (the "widths" command). One global,
// shared by the draw TU and the REPL.
inline std::map<uint32_t, int> g_widthOverrides;


int terminalWidthImpl();
uint32_t utf8FirstCp(const std::string& s);
int cpWidth(uint32_t cp);
int displayWidth(const std::string& s);
std::string clip(const std::string& s, int width);
std::vector<std::string> wrapTextImpl(const std::string& s, int width,
                                      int firstW = -1);
std::string pad(const std::string& s, int width);
std::string repeat(char c, size_t n);
std::string highlightMentions(const std::string& body);
std::string eventBodyImpl(const matrix::Event& ev);
std::string eventThreadRoot(const matrix::Event& ev);
std::string eventReplyTo(const matrix::Event& ev);
int roomThreadCount(db::Database* db, const std::string& roomId);
int64_t parseDayMsImpl(const std::string& s);
std::string eventBodyRaw(const matrix::Event& ev);
std::string roomDisplayNameImpl(const UiState& st, const nlohmann::json& r);
// The single room-resolution used by every user-input path: matches the
// room id, the canonical alias (#design) and the display name, exact,
// prefix or substring, case-insensitively. Returns "" when nothing
// matched (never the input itself).
std::string matchRoomInCache(const std::vector<nlohmann::json>& rooms,
                             const std::string& query);
int roomMessageCount(db::Database* db, const std::string& roomId);
std::string senderShortImpl(const std::string& sender);
std::string eventPreview(db::Database* db, const std::string& roomId,
                         const std::string& eventId);
std::string roomLastTime(db::Database* db, const std::string& roomId,
                         bool seconds, bool clock12h);
int64_t roomLastTs(db::Database* db, const std::string& roomId);
std::string renderPermalinks(const std::string& body,
                             const std::vector<nlohmann::json>& rooms,
                             db::Database* db);
std::string highlightUrls(const std::string& text);
std::string roomLastMsg(db::Database* db, const std::string& roomId,
                        const std::vector<nlohmann::json>& rooms);
matrix::Event roomLastEvent(db::Database* db, const std::string& roomId);
int centerRowIndexOfImpl(const UiState& st, const std::string& eventId);
// The m.room.tombstone successor of the room ("" when none) — the receive
// side of a room upgrade.
std::string tombstoneSuccessor(const std::vector<matrix::Event>& events);
void sortRoomsByActivity(UiState& st);
std::string proxyLabelText();
int contentRowsImpl(const UiState& st);
void loadRoomIntoStateImpl(UiState& st, const std::string& query);
std::string resolveThreadRoot(db::Database* db, const std::string& roomId,
                              const std::string& hint);
// The room's join rule ("public" / "invite" / ...) — the openness sign
// on the invite rows.
std::string roomJoinRule(db::Database* db, const std::string& roomId);
// The shared room-info printer used by `info` and `demo <room> info`
// (name, alias, id, topic, members, direct, space, E2EE, version, creator,
// join rule, message/unread counts, last activity).
void printRoomInfo(db::Database* db, const nlohmann::json& room);
std::vector<std::string> roomThreadList(db::Database* db,
                                        const std::string& roomId, int clipW,
                                        bool showIds);
std::string resolveSpace(const std::vector<nlohmann::json>& rooms,
                         const std::string& query);
std::string viaSuffix(db::Database* db, const std::string& roomId, int limit);
// Distinct federated server domains (sender domains) seen in a room's cache,
// ordered by first appearance.
std::vector<std::string> viaServers(db::Database* db, const std::string& roomId);
// Total number of those servers — i.e. the maximum value --via can usefully
// be set to.
int viaServerCount(db::Database* db, const std::string& roomId);
// Terminal width in columns (for permalink fitting); falls back to COLUMNS
// env, then 100.
int terminalColumns();
std::string displayName(const UiState& st, const std::string& roomId,
                        const std::string& sender);
std::string chatName(const UiState& st, const std::string& roomId,
                     const std::string& sender);
std::string chatNameColoured(const UiState& st, const std::string& roomId,
                             const std::string& sender);
std::string highlightCodeLine(const std::string& line, bool cFamily);
std::string renderMarkdownBody(const std::string& body);
std::string senderTag(const UiState& st, const std::string& roomId,
                      const std::string& sender);
std::string fullMxid(const UiState& st, const std::string& mem);
std::string ansiColourCode(const std::string& name);
void applyColourSpec(const std::string& spec, std::string (&dst)[3]);
std::string colourOpen(const std::string& code, bool bold);
std::string memberRowStr(const UiState& st, const std::string& mem,
                         bool fullIds = false, int panel = 2);
std::string drawFrameImpl(const UiState& st);
// The chat panel's rows (ascii_ui_draw_center.cpp) — split out of the
// frame builder so every translation unit stays under ~1000 lines.
std::vector<std::string> buildCenterRows(const UiState& st, int centerW, int W,
                                         bool horizMembers);
std::string drawFrameChatImpl(const UiState& st, int centerW, bool horizMembers,
                              int W, int leftW, int rightW, int scroll,
                              int rows, const char* PIPE, const char* X,
                              std::string out,
                              const std::vector<const nlohmann::json*>& visible,
                              const std::string& roomName);

// The REPL dispatchers (return 0 = unhandled, 1 = handled, 2 = quit).
int asciiReplDispatchA(UiState& st, db::Database& dbi, const cli::Args& a);
int asciiReplDispatchB(UiState& st, db::Database& dbi, const cli::Args& a);
int asciiReplDispatchG(UiState& st, db::Database& dbi, const cli::Args& a);
int asciiAgentReplDispatch(UiState& st, db::Database& dbi, const cli::Args& a);
int asciiReplDispatchE(UiState& st, db::Database& dbi, const cli::Args& a);

} // namespace matrixcli
