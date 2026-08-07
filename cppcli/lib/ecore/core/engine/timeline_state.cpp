// src/core/engine/timeline_state.cpp
#include "timeline_state.hpp"
#include "../debug_log.hpp"

#include <algorithm>

namespace progressive::desktop {

namespace {
// 5-minute same-sender merge window (view-grouping semantics — see the hpp).
constexpr int64_t kMergeWindowMs = 300000;
} // namespace

std::string TimelineState::normEmoji(const std::string& e) {
    std::string out;
    for (size_t i = 0; i < e.size(); ++i) {
        unsigned char c = e[i];
        if (c == 0xEF && i + 2 < e.size() && e[i+1] == 0xB8 && e[i+2] == 0x8F) {
            i += 2; continue;
        }
        if (c == 0xE2 && i + 2 < e.size() && e[i+1] == 0x80 && e[i+2] == 0x8D) {
            i += 2; continue;
        }
        out += c;
    }
    return out;
}

void TimelineState::updateGroupMarkers(std::vector<DisplayedEvent>& events) {
    for (size_t i = 0; i < events.size(); ++i) {
        auto& e = events[i];
        bool isSystem = (e.type == "m.room.member" || e.type == "m.room.redaction");
        bool isEmote = (e.msgtype == "m.emote");
        if (isSystem || isEmote) { e.groupFirst = true; e.groupLast = true; continue; }
        bool first = true;
        for (int p = static_cast<int>(i) - 1; p >= 0; --p) {
            auto& prev = events[static_cast<size_t>(p)];
            if (prev.type != "m.room.member" && prev.type != "m.room.redaction" && prev.msgtype != "m.emote") {
                int64_t gap = e.originServerTs - prev.originServerTs;
                first = (prev.senderId != e.senderId || gap > kMergeWindowMs || gap < -kMergeWindowMs);
                break;
            }
        }
        bool last = true;
        for (size_t n = i + 1; n < events.size(); ++n) {
            auto& next = events[n];
            if (next.type != "m.room.member" && next.type != "m.room.redaction" && next.msgtype != "m.emote") {
                int64_t gap = next.originServerTs - e.originServerTs;
                last = (next.senderId != e.senderId || gap > kMergeWindowMs || gap < -kMergeWindowMs);
                break;
            }
        }
        e.groupFirst = first;
        e.groupLast = last;
    }
}

TimelineState::AppendResult TimelineState::appendBack(const DisplayedEvent& evt) {
    AppendResult r;
    if (!evt.eventId.empty() && seenIds_.count(evt.eventId)) return r;
    if (!evt.eventId.empty()) seenIds_.insert(evt.eventId);

    int row = static_cast<int>(events_.size());
    events_.push_back(evt);
    r.changed = true;
    r.insertedRow = row;
    if (!evt.eventId.empty()) rowIndex_[evt.eventId] = row;
    // Reactions (incl. decrypted m.reaction events) are never thread replies.
    if (evt.isThreadReply && !evt.threadRootId.empty() && evt.type != "m.reaction") {
        auto it = rowIndex_.find(evt.threadRootId);
        if (it != rowIndex_.end() && it->second >= 0 && it->second < static_cast<int>(events_.size())) {
            events_[static_cast<size_t>(it->second)].threadReplyCount++;
            r.threadRootRow = it->second;
        }
    }
    LOG(LogChannel::DBG, "appendBack: eid=%s eventIdEmpty=%d type=%s threadReply=%d",
        evt.eventId.c_str(), (int)evt.eventId.empty(), evt.type.c_str(),
        evt.isThreadReply ? (int)!evt.threadRootId.empty() : -1);
    updateGroupMarkers(events_);

    if (static_cast<int>(events_.size()) > MAX_TIMELINE_EVENTS) {
        int excess = static_cast<int>(events_.size()) - MAX_TIMELINE_EVENTS;
        for (int i = 0; i < excess; ++i) {
            seenIds_.erase(events_[static_cast<size_t>(i)].eventId);
        }
        events_.erase(events_.begin(), events_.begin() + excess);
        r.evictedCount = excess;
        rowIndex_.clear();
        for (size_t i = 0; i < events_.size(); ++i) {
            if (!events_[i].eventId.empty()) rowIndex_[events_[i].eventId] = static_cast<int>(i);
        }
    }
    return r;
}

TimelineState::AppendResult TimelineState::appendBackBatch(const std::vector<DisplayedEvent>& events) {
    AppendResult r;
    std::vector<DisplayedEvent> filtered;
    for (const auto& evt : events) {
        if (!evt.eventId.empty() && seenIds_.count(evt.eventId)) continue;
        if (!evt.eventId.empty()) seenIds_.insert(evt.eventId);
        filtered.push_back(evt);
    }
    if (filtered.empty()) return r;

    r.changed = true;
    r.firstRow = static_cast<int>(events_.size());
    events_.insert(events_.end(), filtered.begin(), filtered.end());
    r.lastRow = static_cast<int>(events_.size()) - 1;

    rowIndex_.clear();
    for (size_t i = 0; i < events_.size(); ++i) {
        if (!events_[i].eventId.empty()) rowIndex_[events_[i].eventId] = static_cast<int>(i);
    }
    for (const auto& evt : filtered) {
        if (evt.isThreadReply && !evt.threadRootId.empty()) {
            auto it = rowIndex_.find(evt.threadRootId);
            if (it != rowIndex_.end() && it->second >= 0 && it->second < static_cast<int>(events_.size())) {
                events_[static_cast<size_t>(it->second)].threadReplyCount++;
                r.threadRootRow = it->second;
            }
        }
    }
    updateGroupMarkers(events_);
    return r;
}

int TimelineState::appendFront(const std::vector<DisplayedEvent>& evts) {
    std::vector<DisplayedEvent> newOnes;
    for (const auto& e : evts) {
        if (e.eventId.empty() || !seenIds_.count(e.eventId)) {
            if (!e.eventId.empty()) seenIds_.insert(e.eventId);
            newOnes.push_back(e);
        }
    }
    if (newOnes.empty()) return 0;
    events_.insert(events_.begin(), newOnes.rbegin(), newOnes.rend());
    rowIndex_.clear();
    for (size_t i = 0; i < events_.size(); ++i) {
        if (!events_[i].eventId.empty()) rowIndex_[events_[i].eventId] = static_cast<int>(i);
    }
    for (const auto& evt : newOnes) {
        if (evt.isThreadReply && !evt.threadRootId.empty()) {
            auto it = rowIndex_.find(evt.threadRootId);
            if (it != rowIndex_.end() && it->second >= 0 && it->second < static_cast<int>(events_.size())) {
                events_[static_cast<size_t>(it->second)].threadReplyCount++;
            }
        }
    }
    updateGroupMarkers(events_);
    return static_cast<int>(newOnes.size());
}

TimelineState::EchoResult TimelineState::replaceEcho(const std::string& tempEventId,
                                                      const DisplayedEvent& realEvent) {
    auto rit = rowIndex_.find(tempEventId);
    if (rit == rowIndex_.end()) {
        appendBack(realEvent);
        return EchoResult::Appended;
    }
    int i = rit->second;
    if (i < 0 || i >= static_cast<int>(events_.size())) {
        appendBack(realEvent);
        return EchoResult::Appended;
    }
    rowIndex_.erase(tempEventId);
    if (!realEvent.eventId.empty() && seenIds_.count(realEvent.eventId)) {
        events_.erase(events_.begin() + i);
        rowIndex_.clear();
        for (size_t j = 0; j < events_.size(); ++j) {
            if (!events_[j].eventId.empty()) rowIndex_[events_[j].eventId] = static_cast<int>(j);
        }
        return EchoResult::RemovedDuplicate;
    }
    events_[static_cast<size_t>(i)] = realEvent;
    if (!realEvent.eventId.empty()) {
        seenIds_.insert(realEvent.eventId);
        rowIndex_[realEvent.eventId] = i;
    }
    return EchoResult::Replaced;
}

bool TimelineState::markDeleted(const std::string& eventId) {
    int row = findRow(eventId);
    if (row < 0) return false;
    auto& e = events_[static_cast<size_t>(row)];
    e.body = "[Message deleted]";
    e.msgtype = "m.notice";
    e.mxcUrl.clear();
    return true;
}

bool TimelineState::updateBody(const std::string& eventId, const std::string& newBody) {
    int row = findRow(eventId);
    if (row < 0) return false;
    auto& e = events_[static_cast<size_t>(row)];
    e.body = newBody;
    e.msgtype = "m.text";  // edited messages are always m.text
    return true;
}

bool TimelineState::replaceEvent(const std::string& eventId, const DisplayedEvent& newEvent) {
    int row = findRow(eventId);
    if (row < 0) return false;
    auto& e = events_[static_cast<size_t>(row)];
    e.body = newEvent.body;
    e.msgtype = newEvent.msgtype;
    e.contentJson = newEvent.contentJson;
    e.mxcUrl = newEvent.mxcUrl;
    e.mimetype = newEvent.mimetype;
    e.isThreadRoot = newEvent.isThreadRoot;
    e.threadReplyCount = newEvent.threadReplyCount;
    e.isThreadReply = newEvent.isThreadReply;
    e.threadRootId = newEvent.threadRootId;
    e.isMovie = newEvent.isMovie;
    return true;
}

void TimelineState::clear() {
    events_.clear();
    seenIds_.clear();
    rowIndex_.clear();
}

bool TimelineState::addReaction(const std::string& eventId, const std::string& emoji,
                                const std::string& userId, const std::string& reactionEventId) {
    int row = findRow(eventId);
    if (row < 0) return false;
    auto& reactions = events_[static_cast<size_t>(row)].reactions;
    for (auto& r : reactions) {
        if (normEmoji(r.emoji) == normEmoji(emoji)) {
            for (const auto& u : r.userIds) {
                if (u == userId) return false;
            }
            r.count++;
            r.userIds.push_back(userId);
            if (!reactionEventId.empty()) r.reactionEventId = reactionEventId;
            return true;
        }
    }
    reactions.push_back({emoji, 1, false, {userId}, reactionEventId});
    return true;
}

bool TimelineState::removeReaction(const std::string& eventId, const std::string& emoji,
                                   const std::string& userId) {
    int row = findRow(eventId);
    if (row < 0) return false;
    auto& reactions = events_[static_cast<size_t>(row)].reactions;
    for (auto it = reactions.begin(); it != reactions.end(); ++it) {
        if (normEmoji(it->emoji) == normEmoji(emoji)) {
            auto& users = it->userIds;
            users.erase(std::remove(users.begin(), users.end(), userId), users.end());
            it->count = static_cast<int>(users.size());
            if (it->count <= 0) reactions.erase(it);
            return true;
        }
    }
    return false;
}

std::string TimelineState::myReactionId(const std::string& eventId, const std::string& emoji,
                                        const std::string& myUserId) const {
    int row = findRow(eventId);
    if (row < 0) return {};
    for (const auto& r : events_[static_cast<size_t>(row)].reactions) {
        if (normEmoji(r.emoji) == normEmoji(emoji)) {
            for (const auto& u : r.userIds) {
                if (u == myUserId) return r.reactionEventId;
            }
        }
    }
    return {};
}

bool TimelineState::setPinned(const std::string& eventId, bool pinned) {
    int row = findRow(eventId);
    if (row < 0) return false;
    events_[static_cast<size_t>(row)].isPinned = pinned;
    return true;
}

int TimelineState::findRow(const std::string& eventId) const {
    auto it = rowIndex_.find(eventId);
    return it != rowIndex_.end() ? it->second : -1;
}

const DisplayedEvent* TimelineState::at(int row) const {
    if (row < 0 || row >= static_cast<int>(events_.size())) return nullptr;
    return &events_[static_cast<size_t>(row)];
}

DisplayedEvent* TimelineState::at(int row) {
    if (row < 0 || row >= static_cast<int>(events_.size())) return nullptr;
    return &events_[static_cast<size_t>(row)];
}

} // namespace progressive::desktop
