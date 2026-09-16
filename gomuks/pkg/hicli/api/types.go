// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

type RoomInfo struct {
	ID          id.RoomID  `json:"id"`
	Name        string     `json:"name"`
	Topic       string     `json:"topic,omitempty"`
	AvatarURL   string     `json:"avatar_url,omitempty"`
	MemberCount int        `json:"member_count"`
	UnreadCount int        `json:"unread_count"`
	LastEvent   *EventInfo `json:"last_event,omitempty"`
}

type EventInfo struct {
	ID            id.EventID     `json:"id"`
	Sender        id.UserID      `json:"sender"`
	SenderName    string         `json:"sender_name"`
	Timestamp     int64          `json:"timestamp"`
	Type          event.Type     `json:"type"`
	Body          string         `json:"body"`
	Format        string         `json:"format,omitempty"`
	FormattedBody string         `json:"formatted_body,omitempty"`
	MsgType       string         `json:"msgtype,omitempty"`
	URL           string         `json:"url,omitempty"`
	Info          map[string]any `json:"info,omitempty"`
}

type RoomMessages struct {
	RoomID    id.RoomID   `json:"room_id"`
	Messages  []EventInfo `json:"messages"`
	NextBatch string      `json:"next_batch,omitempty"`
}

type RoomList struct {
	Rooms []RoomInfo `json:"rooms"`
}

type ClientStatus struct {
	LoggedIn       bool   `json:"logged_in"`
	UserID         string `json:"user_id,omitempty"`
	HomeserverURL  string `json:"homeserver_url,omitempty"`
	SyncStatus     string `json:"sync_status"`
	ConnectionType string `json:"connection_type,omitempty"`
}
