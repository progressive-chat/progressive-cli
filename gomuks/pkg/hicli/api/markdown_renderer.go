// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"bytes"
	"context"
	"fmt"
	"strconv"
	"strings"
	"time"

	"maunium.net/go/mautrix/id"
)

type MarkdownRenderer struct{}

func (r *MarkdownRenderer) ContentType() string {
	return "text/markdown; charset=utf-8"
}

func (r *MarkdownRenderer) Render(ctx context.Context, data interface{}) ([]byte, error) {
	var buf bytes.Buffer
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
		return nil, fmt.Errorf("unsupported type: %T", data)
	}
	return buf.Bytes(), nil
}

func (r *MarkdownRenderer) renderClientStatus(buf *bytes.Buffer, cs *ClientStatus) {
	status := "Logged out"
	if cs.LoggedIn {
		status = "Logged in"
	}
	rows := [][]string{
		{"Status", status},
		{"User", cs.UserID},
		{"Homeserver", cs.HomeserverURL},
		{"Sync", cs.SyncStatus},
		{"Connection", cs.ConnectionType},
	}
	buf.WriteString("# Matrix Client Status\n\n")
	buf.WriteString(mdTable([]string{"Field", "Value"}, rows))
}

func (r *MarkdownRenderer) renderRoomList(buf *bytes.Buffer, rl *RoomList) {
	headers := []string{"#", "Room", "Topic", "Members", "Unread"}
	var rows [][]string
	for i, room := range rl.Rooms {
		rows = append(rows, []string{
			strconv.Itoa(i + 1),
			escapeMD(room.Name),
			escapeMD(room.Topic),
			strconv.Itoa(room.MemberCount),
			strconv.Itoa(room.UnreadCount),
		})
	}
	buf.WriteString("# Rooms\n\n")
	buf.WriteString(mdTable(headers, rows))
}

func (r *MarkdownRenderer) renderRoomMessages(buf *bytes.Buffer, rm *RoomMessages) {
	fmt.Fprintf(buf, "### Room: %s\n\n", rm.RoomID)
	for i := range rm.Messages {
		r.renderMessage(buf, &rm.Messages[i], "15:04")
	}
}

func (r *MarkdownRenderer) renderMessage(buf *bytes.Buffer, evt *EventInfo, timeFormat string) {
	ts := time.UnixMilli(evt.Timestamp).UTC().Format(timeFormat)

	switch evt.MsgType {
	case "m.emote":
		fmt.Fprintf(buf, "%s — %s\n\n",
			mdItalic(escapeMD(evt.Body)), mdItalic(ts))
	case "m.image":
		fmt.Fprintf(buf, "![%s](%s) — %s\n\n",
			escapeMD(evt.Body), evt.URL, mdItalic(ts))
	case "m.file", "m.audio", "m.video":
		fmt.Fprintf(buf, "[%s](%s) — %s\n\n",
			escapeMD(evt.Body), evt.URL, mdItalic(ts))
	default:
		sender := formatSender(evt.Sender, evt.SenderName)
		body := escapeMD(evt.Body)
		if evt.MsgType == "m.notice" {
			body = "> " + body
		}
		fmt.Fprintf(buf, "%s — %s  \n%s\n\n",
			sender, mdItalic(ts), body)
	}
}

func (r *MarkdownRenderer) renderRoomInfo(buf *bytes.Buffer, ri *RoomInfo) {
	fmt.Fprintf(buf, "# %s\n\n", escapeMD(ri.Name))
	if ri.Topic != "" {
		fmt.Fprintf(buf, "> %s\n\n", escapeMD(ri.Topic))
	}
	buf.WriteString(fmt.Sprintf("- **Members:** %d\n", ri.MemberCount))
	buf.WriteString(fmt.Sprintf("- **Room ID:** %s\n", string(ri.ID)))
}

func (r *MarkdownRenderer) renderEventInfo(buf *bytes.Buffer, ei *EventInfo) {
	ts := time.UnixMilli(ei.Timestamp).UTC().Format("15:04:05")

	switch ei.MsgType {
	case "m.emote":
		fmt.Fprintf(buf, "%s — %s\n",
			mdItalic(escapeMD(ei.Body)), mdItalic(ts))
	case "m.image":
		fmt.Fprintf(buf, "![%s](%s) — %s\n",
			escapeMD(ei.Body), ei.URL, mdItalic(ts))
	case "m.file", "m.audio", "m.video":
		fmt.Fprintf(buf, "[%s](%s) — %s\n",
			escapeMD(ei.Body), ei.URL, mdItalic(ts))
	default:
		sender := formatSender(ei.Sender, ei.SenderName)
		body := escapeMD(ei.Body)
		if ei.MsgType == "m.notice" {
			body = "> " + body
		}
		fmt.Fprintf(buf, "%s — %s  \n%s\n",
			sender, mdItalic(ts), body)
	}
}

func formatSender(sender id.UserID, senderName string) string {
	s := string(sender)
	lp := localpart(s)
	if senderName != "" && !strings.EqualFold(senderName, lp) {
		return mdBold("@" + lp) + " (" + senderName + ")"
	}
	return mdBold("@" + lp)
}

func localpart(userID string) string {
	if len(userID) == 0 || userID[0] != '@' {
		return userID
	}
	rest := userID[1:]
	if idx := strings.IndexByte(rest, ':'); idx >= 0 {
		return rest[:idx]
	}
	return rest
}

func escapeMD(s string) string {
	s = strings.ReplaceAll(s, "\\", "\\\\")
	s = strings.ReplaceAll(s, "*", "\\*")
	s = strings.ReplaceAll(s, "_", "\\_")
	s = strings.ReplaceAll(s, "`", "\\`")
	s = strings.ReplaceAll(s, "[", "\\[")
	s = strings.ReplaceAll(s, "]", "\\]")
	s = strings.ReplaceAll(s, "<", "\\<")
	s = strings.ReplaceAll(s, ">", "\\>")
	s = strings.ReplaceAll(s, "#", "\\#")
	s = strings.ReplaceAll(s, "|", "\\|")
	return s
}

func mdTable(headers []string, rows [][]string) string {
	cols := len(headers)
	widths := make([]int, cols)
	for i, h := range headers {
		widths[i] = len(h)
	}
	for _, row := range rows {
		for i, cell := range row {
			if i < cols && len(cell) > widths[i] {
				widths[i] = len(cell)
			}
		}
	}
	var b strings.Builder
	writeRow(&b, headers, widths)
	writeSep(&b, widths)
	for _, row := range rows {
		writeRow(&b, row, widths)
	}
	return b.String()
}

func writeRow(b *strings.Builder, cells []string, widths []int) {
	b.WriteByte('|')
	for i, cell := range cells {
		if i < len(widths) {
			fmt.Fprintf(b, " %-*s |", widths[i], cell)
		}
	}
	b.WriteByte('\n')
}

func writeSep(b *strings.Builder, widths []int) {
	b.WriteByte('|')
	for _, w := range widths {
		for i := 0; i < w+2; i++ {
			b.WriteByte('-')
		}
		b.WriteByte('|')
	}
	b.WriteByte('\n')
}

func mdBold(s string) string {
	return "**" + s + "**"
}

func mdItalic(s string) string {
	return "*" + s + "*"
}

func mdCode(s string) string {
	return "`" + s + "`"
}

func init() {
	RegisterRenderer(FormatMarkdown, &MarkdownRenderer{})
}
