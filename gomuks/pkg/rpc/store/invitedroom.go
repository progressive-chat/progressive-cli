// Copyright (c) 2025 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package store

import (
	"time"

	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"

	"go.mau.fi/gomuks/pkg/hicli/database"
)

type InvitedRoom struct {
	*RoomListEntry
	parent         *GomuksStore
	RoomVersion    id.RoomVersion
	Encryption     id.Algorithm
	JoinRule       event.JoinRule
	Topic          string
	CanonicalAlias id.RoomAlias
	InvitedBy      id.UserID
	InviterProfile *event.MemberEventContent
	IsDirect       bool
}

const inviteTimeOffset = 1_000_000 * time.Hour

func NewInvitedRoom(meta *database.InvitedRoom, parent *GomuksStore) *InvitedRoom {
	entry := &InvitedRoom{
		parent: parent,
		RoomListEntry: &RoomListEntry{
			RoomID:           meta.ID,
			SortingTimestamp: meta.CreatedAt.Add(inviteTimeOffset),
			IsInvite:         true,
			UnreadCounts:     database.UnreadCounts{UnreadHighlights: 1},
		},
	}
	members := make(map[id.UserID]*event.Event, 2)
	for _, evt := range meta.InviteState {
		evt.Type.Class = event.StateEventType
		switch evt.Type {
		case event.StateRoomName:
			entry.Name, _ = evt.Content.Raw["name"].(string)
		case event.StateRoomAvatar:
			avatarURL, _ := evt.Content.Raw["url"].(string)
			entry.Avatar, _ = id.ParseContentURI(avatarURL)
		case event.StateCanonicalAlias:
			alias, _ := evt.Content.Raw["alias"].(string)
			entry.CanonicalAlias = id.RoomAlias(alias)
		case event.StateTopic:
			entry.Topic, _ = evt.Content.Raw["topic"].(string)
		case event.StateEncryption:
			alg, _ := evt.Content.Raw["algorithm"].(string)
			entry.Encryption = id.Algorithm(alg)
		case event.StateCreate:
			roomVersion, _ := evt.Content.Raw["room_version"].(string)
			entry.RoomVersion = id.RoomVersion(roomVersion)
		case event.StateMember:
			_ = evt.Content.ParseRaw(evt.Type)
			members[id.UserID(*evt.StateKey)] = evt
		case event.StateJoinRules:
			joinRule, _ := evt.Content.Raw["join_rule"].(string)
			entry.JoinRule = event.JoinRule(joinRule)
		}
	}
	ownMemberEvt, ok := members[parent.UserID]
	if ok {
		content := ownMemberEvt.Content.AsMember()
		entry.IsDirect = content.IsDirect
		entry.InvitedBy = ownMemberEvt.Sender
		inviterEvt, ok := members[entry.InvitedBy]
		if ok {
			entry.InviterProfile = inviterEvt.Content.AsMember()
		}
	}
	if entry.Name == "" &&
		entry.Avatar.IsEmpty() &&
		entry.Topic == "" &&
		entry.CanonicalAlias == "" &&
		entry.JoinRule == event.JoinRuleInvite &&
		entry.InvitedBy != "" && entry.IsDirect {
		entry.DMUserID = entry.InvitedBy
		if entry.InviterProfile != nil {
			entry.Name = entry.InviterProfile.Displayname
			entry.Avatar = entry.InviterProfile.AvatarURL.ParseOrIgnore()
		}
		if entry.Name == "" {
			entry.Name = entry.InvitedBy.Localpart()
		}
	}
	entry.SearchName = toSearchableString(entry.Name)
	return entry
}
