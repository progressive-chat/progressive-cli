// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
	"time"

	"maunium.net/go/mautrix/id"
)

type TextRenderer struct{}

func (r *TextRenderer) ContentType() string {
	return "text/plain"
}

func (r *TextRenderer) Render(ctx context.Context, data interface{}) ([]byte, error) {
	var out strings.Builder
	switch v := data.(type) {
	case *ClientStatus:
		r.renderClientStatus(&out, *v)
	case ClientStatus:
		r.renderClientStatus(&out, v)
	case *RoomList:
		r.renderRoomList(&out, *v)
	case RoomList:
		r.renderRoomList(&out, v)
	case *RoomMessages:
		r.renderRoomMessages(&out, *v)
	case RoomMessages:
		r.renderRoomMessages(&out, v)
	case *RoomInfo:
		r.renderRoomInfo(&out, *v)
	case RoomInfo:
		r.renderRoomInfo(&out, v)
	case *EventInfo:
		r.renderEventInfo(&out, *v)
	case EventInfo:
		r.renderEventInfo(&out, v)
	default:
		jsonBytes, err := json.MarshalIndent(data, "", "  ")
		if err != nil {
			return nil, err
		}
		_, _ = out.Write(jsonBytes)
	}
	return []byte(out.String()), nil
}

func (r *TextRenderer) renderClientStatus(out *strings.Builder, s ClientStatus) {
	if !s.LoggedIn {
		out.WriteString("Not logged in")
		return
	}
	domain := ""
	if u, err := url.Parse(s.HomeserverURL); err == nil {
		domain = u.Host
	}
	fmt.Fprintf(out, "Logged in as %s on %s [%s]", s.UserID, domain, s.SyncStatus)
}

func (r *TextRenderer) renderRoomList(out *strings.Builder, rl RoomList) {
	fmt.Fprintf(out, "Rooms (%d):\n", len(rl.Rooms))
	for _, room := range rl.Rooms {
		name := room.Name
		if name == "" {
			name = room.ID.String()
		}
		prefix := "  "
		if room.UnreadCount > 0 {
			prefix = "  * "
		}
		topicPart := ""
		if room.Topic != "" {
			topicPart = " — " + room.Topic
		}
		unreadPart := ""
		if room.UnreadCount > 0 {
			unreadPart = fmt.Sprintf(" (%d unread)", room.UnreadCount)
		}
		fmt.Fprintf(out, "%s%s%s%s\n", prefix, name, topicPart, unreadPart)
	}
}

func (r *TextRenderer) renderRoomMessages(out *strings.Builder, rm RoomMessages) {
	for _, evt := range rm.Messages {
		r.renderEvent(out, evt)
		out.WriteByte('\n')
	}
}

func (r *TextRenderer) renderRoomInfo(out *strings.Builder, ri RoomInfo) {
	fmt.Fprintf(out, "Room: %s\n", ri.ID)
	if ri.Topic != "" {
		fmt.Fprintf(out, "Topic: %s\n", ri.Topic)
	}
	fmt.Fprintf(out, "Members: %d\n", ri.MemberCount)
}

func (r *TextRenderer) renderEventInfo(out *strings.Builder, ei EventInfo) {
	r.renderEvent(out, ei)
}

func (r *TextRenderer) renderEvent(out *strings.Builder, ei EventInfo) {
	sender := getSender(ei.Sender, ei.SenderName)
	ts := formatEventTimestamp(ei.Timestamp)
	body := formatEventBody(ei.MsgType, ei.Body, ei.Info)
	switch ei.MsgType {
	case "m.emote":
		fmt.Fprintf(out, "[%s] * %s %s", ts, sender, body)
	case "m.notice":
		fmt.Fprintf(out, "[%s] --- %s: %s", ts, sender, body)
	default:
		fmt.Fprintf(out, "[%s] %s: %s", ts, sender, body)
	}
}

func formatEventBody(msgType string, rawBody string, info map[string]any) string {
	switch msgType {
	case "m.image":
		return fmt.Sprintf("[image: %s]", formatFilename(rawBody, info))
	case "m.file":
		return fmt.Sprintf("[file: %s]", formatFileInfo(rawBody, info))
	case "m.audio":
		return fmt.Sprintf("[audio: %s]", formatFilename(rawBody, info))
	case "m.video":
		return fmt.Sprintf("[video: %s]", formatFilename(rawBody, info))
	default:
		return rawBody
	}
}

func formatFilename(body string, info map[string]any) string {
	if body != "" {
		return body
	}
	if info != nil {
		if name, ok := info["filename"].(string); ok && name != "" {
			return name
		}
	}
	return "unknown"
}

func formatFileInfo(body string, info map[string]any) string {
	name := formatFilename(body, info)
	sizeStr := ""
	if info != nil {
		if size, ok := info["size"].(float64); ok && size > 0 {
			sizeStr = formatSize(int64(size))
		}
	}
	if sizeStr != "" {
		return fmt.Sprintf("%s (%s)", name, sizeStr)
	}
	return name
}

func formatSize(size int64) string {
	const (
		KB = 1024
		MB = 1024 * KB
		GB = 1024 * MB
	)
	switch {
	case size >= GB:
		return fmt.Sprintf("%.1f GB", float64(size)/float64(GB))
	case size >= MB:
		return fmt.Sprintf("%.1f MB", float64(size)/float64(MB))
	case size >= KB:
		return fmt.Sprintf("%.1f KB", float64(size)/float64(KB))
	default:
		return fmt.Sprintf("%d B", size)
	}
}

func formatEventTimestamp(ts int64) string {
	t := time.UnixMilli(ts)
	now := time.Now()
	if t.Year() == now.Year() && t.YearDay() == now.YearDay() {
		return t.Format("15:04:05")
	}
	return t.Format("2006-01-02 15:04:05")
}

func getSender(sender id.UserID, name string) string {
	if name != "" && name != sender.Localpart() {
		return fmt.Sprintf("@%s (%s)", sender.Localpart(), name)
	}
	return fmt.Sprintf("@%s", sender.Localpart())
}

func init() {
	RegisterRenderer(FormatText, &TextRenderer{})
}
