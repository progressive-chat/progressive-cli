// gomuks - A Matrix client written in Go.
// Copyright (C) 2026 Tulir Asokan
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

package gomuks

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"time"

	"go.mau.fi/util/ptr"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"

	"go.mau.fi/gomuks/pkg/hicli/api"
	"go.mau.fi/gomuks/pkg/hicli/database"
)

func (gmx *Gomuks) handleAPI(w http.ResponseWriter, r *http.Request, data any) {
	format := api.DetectFormat(r.Header.Get("Accept"), r.URL.Query().Get("format"))
	renderer, ok := api.GetRenderer(format)
	if !ok {
		renderer, _ = api.GetRenderer(api.FormatJSON)
	}
	rendered, err := renderer.Render(r.Context(), data)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", renderer.ContentType())
	w.Write(rendered)
}

func (gmx *Gomuks) HandleAPIStatus(w http.ResponseWriter, r *http.Request) {
	client := gmx.Client
	syncStatus := "waiting"
	if ss := client.SyncStatus.Load(); ss != nil {
		syncStatus = string(ss.Type)
	}
	status := api.ClientStatus{
		LoggedIn:       client.IsLoggedIn(),
		SyncStatus:     syncStatus,
		ConnectionType: "websocket",
	}
	if client.Account != nil {
		status.UserID = client.Account.UserID.String()
		status.HomeserverURL = client.Account.HomeserverURL
	}
	gmx.handleAPI(w, r, status)
}

func (gmx *Gomuks) HandleAPIRooms(w http.ResponseWriter, r *http.Request) {
	client := gmx.Client
	if !client.IsLoggedIn() {
		gmx.handleAPI(w, r, api.RoomList{Rooms: []api.RoomInfo{}})
		return
	}
	rooms, err := client.DB.Room.GetBySortTS(r.Context(), time.Now().Add(365*24*time.Hour), 500)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	roomList := api.RoomList{
		Rooms: make([]api.RoomInfo, 0, len(rooms)),
	}
	for _, room := range rooms {
		roomList.Rooms = append(roomList.Rooms, roomInfoFromDB(room))
	}
	gmx.handleAPI(w, r, roomList)
}

func (gmx *Gomuks) HandleAPIRoomMessages(w http.ResponseWriter, r *http.Request) {
	roomID := r.PathValue("room_id")
	limit := 50
	if l := r.URL.Query().Get("limit"); l != "" {
		if n, err := strconv.Atoi(l); err == nil && n > 0 && n <= 500 {
			limit = n
		}
	}
	before := database.TimelineRowID(0)
	if b := r.URL.Query().Get("before"); b != "" {
		if n, err := strconv.ParseInt(b, 10, 64); err == nil && n > 0 {
			before = database.TimelineRowID(n)
		}
	}
	events, err := gmx.Client.DB.Timeline.Get(r.Context(), id.RoomID(roomID), limit, before)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	msgs := api.RoomMessages{
		RoomID:   id.RoomID(roomID),
		Messages: make([]api.EventInfo, 0, len(events)),
	}
	for _, evt := range events {
		msgs.Messages = append(msgs.Messages, eventInfoFromDB(evt))
	}
	if len(events) == limit {
		last := events[len(events)-1]
		if last.TimelineRowID > 0 {
			msgs.NextBatch = strconv.FormatInt(int64(last.TimelineRowID), 10)
		}
	}
	gmx.handleAPI(w, r, msgs)
}

func (gmx *Gomuks) HandleAPIRoomInfo(w http.ResponseWriter, r *http.Request) {
	roomID := r.PathValue("room_id")
	room, err := gmx.Client.DB.Room.Get(r.Context(), id.RoomID(roomID))
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	} else if room == nil {
		http.Error(w, "room not found", http.StatusNotFound)
		return
	}
	gmx.handleAPI(w, r, roomInfoFromDB(room))
}

func (gmx *Gomuks) HandleAPISendMessage(w http.ResponseWriter, r *http.Request) {
	roomID := r.PathValue("room_id")
	var req struct {
		Body    string `json:"body"`
		MsgType string `json:"msgtype"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, fmt.Sprintf("invalid request body: %v", err), http.StatusBadRequest)
		return
	}
	if req.Body == "" {
		http.Error(w, "body is required", http.StatusBadRequest)
		return
	}
	msgType := event.MsgText
	if req.MsgType != "" {
		msgType = event.MessageType(req.MsgType)
	}
	content := &event.MessageEventContent{
		MsgType: msgType,
		Body:    req.Body,
	}
	dbEvt, err := gmx.Client.Send(
		r.Context(),
		id.RoomID(roomID),
		event.EventMessage,
		content,
		false,
		false,
	)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	gmx.handleAPI(w, r, map[string]string{"event_id": dbEvt.ID.String()})
}

func roomInfoFromDB(room *database.Room) api.RoomInfo {
	info := api.RoomInfo{
		ID:          room.ID,
		Name:        ptr.Val(room.Name),
		Topic:       ptr.Val(room.Topic),
		MemberCount: roomMemberCount(room),
		UnreadCount: room.UnreadMessages,
	}
	if room.Avatar != nil && !room.Avatar.IsEmpty() {
		info.AvatarURL = room.Avatar.String()
	}
	return info
}

func roomMemberCount(room *database.Room) int {
	if room.LazyLoadSummary == nil {
		return 0
	}
	return ptr.Val(room.LazyLoadSummary.JoinedMemberCount) + ptr.Val(room.LazyLoadSummary.InvitedMemberCount)
}

func eventInfoFromDB(evt *database.Event) api.EventInfo {
	body, format, formattedBody, msgType, url := parseEventContent(evt)
	senderName := evt.Sender.String()
	return api.EventInfo{
		ID:            evt.ID,
		Sender:        evt.Sender,
		SenderName:    senderName,
		Timestamp:     evt.Timestamp.UnixMilli(),
		Type:          evt.GetType(),
		Body:          body,
		Format:        format,
		FormattedBody: formattedBody,
		MsgType:       msgType,
		URL:           url,
	}
}

func parseEventContent(evt *database.Event) (body, format, formattedBody, msgType, url string) {
	evtType := evt.GetType()
	content := evt.GetContent()
	switch evtType.Type {
	case event.EventMessage.Type, event.EventSticker.Type:
		var msg event.MessageEventContent
		if err := json.Unmarshal(content, &msg); err == nil {
			body = msg.Body
			format = string(msg.Format)
			formattedBody = msg.FormattedBody
			msgType = string(msg.MsgType)
			if msg.URL != "" {
				url = string(msg.URL)
			} else if msg.File != nil && msg.File.URL != "" {
				url = string(msg.File.URL)
			}
		}
	}
	if body == "" {
		body = evtType.Type
	}
	return
}
