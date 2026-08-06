// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"context"
	"fmt"
	"strings"
	"time"

	"github.com/mattn/go-runewidth"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

const gemtextWidth = 80

type GeminiRenderer struct{}

func (r *GeminiRenderer) ContentType() string {
	return "text/gemini; charset=utf-8"
}

func (r *GeminiRenderer) Render(ctx context.Context, data interface{}) ([]byte, error) {
	var buf strings.Builder
	switch v := data.(type) {
	case *ClientStatus:
		r.renderClientStatus(&buf, v)
	case *RoomList:
		r.renderRoomList(&buf, v)
	case *RoomMessages:
		r.renderRoomMessages(&buf, v)
	case *RoomInfo:
		r.renderRoomInfo(&buf, v)
	case *EventInfo:
		r.renderEventInfo(&buf, v)
	default:
		return nil, fmt.Errorf("gemini renderer: unsupported data type %T", data)
	}
	return []byte(buf.String()), nil
}

func init() {
	RegisterRenderer(FormatGemini, &GeminiRenderer{})
}

// gemtextWrap wraps text at the given width using word boundaries.
func gemtextWrap(s string, width int) string {
	lines := strings.Split(s, "\n")
	for i, line := range lines {
		if runewidth.StringWidth(line) > width {
			lines[i] = wrapLine(line, width)
		}
	}
	return strings.Join(lines, "\n")
}

func wrapLine(line string, width int) string {
	words := strings.Fields(line)
	if len(words) == 0 {
		return ""
	}
	var buf strings.Builder
	lineLen := 0
	for _, word := range words {
		wordLen := runewidth.StringWidth(word)
		if lineLen > 0 && lineLen+1+wordLen > width {
			buf.WriteByte('\n')
			lineLen = 0
		}
		if lineLen > 0 {
			buf.WriteByte(' ')
			lineLen++
		}
		buf.WriteString(word)
		lineLen += wordLen
	}
	return buf.String()
}

// gemtextLink formats a gemtext link line.
func gemtextLink(url, desc string) string {
	if desc != "" {
		return fmt.Sprintf("=> %s %s", url, desc)
	}
	return fmt.Sprintf("=> %s", url)
}

// gemtextTime formats a Unix millisecond timestamp as HH:MM:SS.
func gemtextTime(ts int64) string {
	return time.UnixMilli(ts).UTC().Format("15:04:05")
}

// gemtextTimeShort formats a Unix millisecond timestamp as HH:MM.
func gemtextTimeShort(ts int64) string {
	return time.UnixMilli(ts).UTC().Format("15:04")
}

// gemtextSender formats a sender for display.
// Returns "@localpart" if there is no display name, or "@localpart (Name)" if there is one.
func gemtextSender(sender id.UserID, name string) string {
	localpart := sender.Localpart()
	if name != "" {
		return fmt.Sprintf("@%s (%s)", localpart, name)
	}
	return fmt.Sprintf("@%s", localpart)
}

func (r *GeminiRenderer) renderClientStatus(buf *strings.Builder, cs *ClientStatus) {
	buf.WriteString("# Matrix Client\n\n")
	if cs.UserID != "" {
		fmt.Fprintf(buf, "User: %s\n", cs.UserID)
	}
	if cs.HomeserverURL != "" {
		fmt.Fprintf(buf, "Server: %s\n", cs.HomeserverURL)
	}
	state := "Logged out"
	if cs.LoggedIn {
		state = "Logged in"
		if cs.SyncStatus != "" {
			state += ", " + cs.SyncStatus
		}
	}
	fmt.Fprintf(buf, "State: %s\n", state)
	if cs.ConnectionType != "" {
		fmt.Fprintf(buf, "Connection: %s\n", cs.ConnectionType)
	}
}

func (r *GeminiRenderer) renderRoomList(buf *strings.Builder, rl *RoomList) {
	buf.WriteString("# Room List\n")
	for _, room := range rl.Rooms {
		buf.WriteByte('\n')
		roomName := room.Name
		if roomName == "" {
			roomName = room.ID.String()
		}
		fmt.Fprintf(buf, "=> /room/%s %s\n", room.ID, roomName)
		if room.Topic != "" {
			fmt.Fprintf(buf, "> %s\n", gemtextWrap(room.Topic, gemtextWidth-2))
		}
		fmt.Fprintf(buf, "Members: %d | Unread: %d\n", room.MemberCount, room.UnreadCount)
	}
}

func (r *GeminiRenderer) renderRoomMessages(buf *strings.Builder, rm *RoomMessages) {
	fmt.Fprintf(buf, "## %s\n", rm.RoomID)
	for i, msg := range rm.Messages {
		if i > 0 {
			buf.WriteByte('\n')
		}
		buf.WriteByte('\n')
		r.renderMessageInList(buf, &msg)
	}
}

func (r *GeminiRenderer) renderMessageInList(buf *strings.Builder, ei *EventInfo) {
	switch ei.MsgType {
	case string(event.MsgImage), string(event.MsgVideo), string(event.MsgAudio), string(event.MsgFile):
		desc := ei.Body
		if desc == "" {
			desc = ei.MsgType
		}
		fmt.Fprintf(buf, "=> %s %s (%s)\n", ei.URL, desc, gemtextTimeShort(ei.Timestamp))
	default:
		fmt.Fprintf(buf, "%s %s\n", gemtextSender(ei.Sender, ei.SenderName), gemtextTimeShort(ei.Timestamp))
		r.renderEventBody(buf, ei)
	}
}

func (r *GeminiRenderer) renderRoomInfo(buf *strings.Builder, ri *RoomInfo) {
	title := ri.Name
	if title == "" {
		title = ri.ID.String()
	}
	fmt.Fprintf(buf, "# %s\n", title)
	if ri.Topic != "" {
		fmt.Fprintf(buf, "> %s\n", gemtextWrap(ri.Topic, gemtextWidth-2))
	}
	buf.WriteByte('\n')
	fmt.Fprintf(buf, "Members: %d\n", ri.MemberCount)
	fmt.Fprintf(buf, "Room ID: %s\n", ri.ID)
}

func (r *GeminiRenderer) renderEventInfo(buf *strings.Builder, ei *EventInfo) {
	fmt.Fprintf(buf, "%s %s\n", gemtextSender(ei.Sender, ei.SenderName), gemtextTime(ei.Timestamp))
	r.renderEventBody(buf, ei)
}

func (r *GeminiRenderer) renderEventBody(buf *strings.Builder, ei *EventInfo) {
	body := ei.Body
	if body == "" {
		return
	}

	hasCodeBlock := strings.Contains(body, "```")

	switch ei.MsgType {
	case string(event.MsgEmote):
		r.renderBodyContent(buf, body, "* ", gemtextWidth-2, hasCodeBlock)
	case string(event.MsgNotice):
		r.renderBodyContent(buf, body, "> ", gemtextWidth-2, hasCodeBlock)
	default:
		r.renderBodyContent(buf, body, "", gemtextWidth, hasCodeBlock)
	}
}

func (r *GeminiRenderer) renderBodyContent(buf *strings.Builder, body, prefix string, width int, hasCodeBlock bool) {
	if !hasCodeBlock {
		for _, line := range strings.Split(body, "\n") {
			if line == "" {
				if prefix != "" {
					buf.WriteString(prefix)
				}
				buf.WriteByte('\n')
			} else {
				fmt.Fprintf(buf, "%s%s\n", prefix, gemtextWrap(line, width))
			}
		}
		return
	}

	parts := strings.Split(body, "```")
	for i, part := range parts {
		trimmed := strings.TrimRight(part, "\n")
		if trimmed == "" {
			continue
		}
		if i%2 == 0 {
			for _, line := range strings.Split(trimmed, "\n") {
				fmt.Fprintf(buf, "%s%s\n", prefix, gemtextWrap(line, width))
			}
		} else {
			code := strings.Trim(trimmed, "\n")
			if code != "" {
				buf.WriteString("```\n")
				buf.WriteString(code)
				buf.WriteByte('\n')
				buf.WriteString("```\n")
			} else {
				buf.WriteString("```\n```\n")
			}
		}
	}
}
