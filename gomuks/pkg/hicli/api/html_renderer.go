// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"context"
	"fmt"
	"html"
	"regexp"
	"strings"
	"time"

	"maunium.net/go/mautrix/id"
)

type HTMLRenderer struct{}

func (r *HTMLRenderer) ContentType() string {
	return "text/html; charset=utf-8"
}

func init() {
	RegisterRenderer(FormatHTML, &HTMLRenderer{})
}

func (r *HTMLRenderer) Render(ctx context.Context, data interface{}) ([]byte, error) {
	var content string
	switch v := data.(type) {
	case *ClientStatus:
		content = r.renderClientStatus(v)
	case *RoomList:
		content = r.renderRoomList(v)
	case *RoomMessages:
		content = r.renderRoomMessages(v)
	case *RoomInfo:
		content = r.renderRoomInfo(v)
	default:
		return nil, fmt.Errorf("HTML renderer: unsupported type %T", data)
	}
	return []byte(fmt.Sprintf(pageTemplate, content)), nil
}

const pageTemplate = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Matrix Client API</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 800px; margin: 0 auto; padding: 1em; background: #1e1e1e; color: #d4d4d4; }
  .room { border: 1px solid #444; border-radius: 8px; margin: 1em 0; padding: 1em; }
  .room h3 { margin: 0 0 0.5em; color: #569cd6; }
  .room .topic { color: #888; font-style: italic; margin-bottom: 1em; }
  .msg { margin: 0.5em 0; padding: 0.5em; border-radius: 4px; }
  .msg:nth-child(even) { background: #2a2a2a; }
  .msg .sender { font-weight: bold; color: #4ec9b0; }
  .msg .time { color: #888; font-size: 0.85em; }
  .msg .body { margin-top: 0.25em; }
  .msg.emote .body { font-style: italic; color: #ce9178; }
  .msg.image .body { color: #569cd6; }
  .msg.file .body { color: #dcdcaa; }
  .msg.audio .body { color: #569cd6; }
  .msg.video .body { color: #dcdcaa; }
  .msg.notice .body { color: #888; border-left: 3px solid #555; padding-left: 0.5em; }
  table { border-collapse: collapse; width: 100%%; }
  th, td { padding: 0.5em; text-align: left; border-bottom: 1px solid #444; }
  th { background: #333; }
  .status { display: flex; gap: 1em; flex-wrap: wrap; }
  .status-item { background: #333; padding: 0.5em 1em; border-radius: 4px; }
  .status-item .label { color: #888; font-size: 0.8em; }
  .status-item .value { font-weight: bold; }
  a { color: #569cd6; }
  code { background: #333; padding: 0.15em 0.3em; border-radius: 3px; }
</style>
</head>
<body>
%s
</body>
</html>`

var urlRegex = regexp.MustCompile(`https?://[^\s<>"']+[^\s<>"',.;:!?)\]}]`)

func htmlEscape(s string) string {
	return html.EscapeString(s)
}

func htmlTime(ts int64) string {
	if ts == 0 {
		return ""
	}
	return time.UnixMilli(ts).Format("15:04:05")
}

func htmlSender(sender id.UserID, name string) string {
	if name != "" {
		return fmt.Sprintf(`<span class="sender">%s (%s)</span>`, htmlEscape(string(sender)), htmlEscape(name))
	}
	return fmt.Sprintf(`<span class="sender">%s</span>`, htmlEscape(string(sender)))
}

func htmlMediaEmoji(msgtype string) string {
	switch msgtype {
	case "m.image":
		return "\U0001F4F7 "
	case "m.file":
		return "\U0001F4C1 "
	case "m.audio":
		return "\U0001F3B5 "
	case "m.video":
		return "\U0001F3AC "
	default:
		return ""
	}
}

func htmlAutoLink(s string) string {
	parts := urlRegex.Split(s, -1)
	matches := urlRegex.FindAllString(s, -1)
	if len(matches) == 0 {
		return htmlEscape(s)
	}
	var buf strings.Builder
	for i, part := range parts {
		buf.WriteString(htmlEscape(part))
		if i < len(matches) {
			u := matches[i]
			buf.WriteString(fmt.Sprintf(`<a href="%s">%s</a>`, htmlEscape(u), htmlEscape(u)))
		}
	}
	return buf.String()
}

func msgTypeClass(msgtype string) string {
	if msgtype == "" {
		return "text"
	}
	if after, ok := strings.CutPrefix(msgtype, "m."); ok {
		return after
	}
	return msgtype
}

func (r *HTMLRenderer) renderClientStatus(s *ClientStatus) string {
	var buf strings.Builder
	buf.WriteString(`<h1>Matrix Client Status</h1>`)
	buf.WriteString(`<div class="status">`)

	userID := "N/A"
	if s.UserID != "" {
		userID = htmlEscape(s.UserID)
	}
	buf.WriteString(fmt.Sprintf(`<div class="status-item"><span class="label">User</span><span class="value">%s</span></div>`, userID))

	homeserver := "N/A"
	if s.HomeserverURL != "" {
		homeserver = htmlEscape(s.HomeserverURL)
	}
	buf.WriteString(fmt.Sprintf(`<div class="status-item"><span class="label">Server</span><span class="value">%s</span></div>`, homeserver))

	statusLabel := "Offline"
	if s.LoggedIn {
		statusLabel = "Online"
	}
	buf.WriteString(fmt.Sprintf(`<div class="status-item"><span class="label">Status</span><span class="value">%s</span></div>`, statusLabel))
	buf.WriteString(fmt.Sprintf(`<div class="status-item"><span class="label">Sync</span><span class="value">%s</span></div>`, htmlEscape(s.SyncStatus)))

	if s.ConnectionType != "" {
		buf.WriteString(fmt.Sprintf(`<div class="status-item"><span class="label">Connection</span><span class="value">%s</span></div>`, htmlEscape(s.ConnectionType)))
	}

	buf.WriteString(`</div>`)
	return buf.String()
}

func (r *HTMLRenderer) renderRoomList(list *RoomList) string {
	var buf strings.Builder
	buf.WriteString(`<h1>Rooms</h1>`)
	buf.WriteString(`<table>`)
	buf.WriteString(`<tr><th>Room</th><th>Topic</th><th>Members</th><th>Unread</th></tr>`)
	for _, room := range list.Rooms {
		name := room.Name
		if name == "" {
			name = string(room.ID)
		}
		buf.WriteString(fmt.Sprintf(`<tr><td><a href="/room/%s">%s</a></td><td>%s</td><td>%d</td><td>%d</td></tr>`,
			htmlEscape(string(room.ID)), htmlEscape(name), htmlEscape(room.Topic), room.MemberCount, room.UnreadCount))
	}
	buf.WriteString(`</table>`)
	return buf.String()
}

func (r *HTMLRenderer) renderRoomMessages(msgs *RoomMessages) string {
	var buf strings.Builder
	buf.WriteString(`<div class="room">`)
	roomName := string(msgs.RoomID)
	buf.WriteString(fmt.Sprintf(`<h3>%s</h3>`, htmlEscape(roomName)))
	for i := range msgs.Messages {
		r.renderEvent(&buf, &msgs.Messages[i])
	}
	buf.WriteString(`</div>`)
	return buf.String()
}

func (r *HTMLRenderer) renderRoomInfo(info *RoomInfo) string {
	var buf strings.Builder
	buf.WriteString(`<div class="room">`)

	name := info.Name
	if name == "" {
		name = string(info.ID)
	}
	buf.WriteString(fmt.Sprintf(`<h1>%s</h1>`, htmlEscape(name)))

	if info.Topic != "" {
		buf.WriteString(fmt.Sprintf(`<div class="topic">%s</div>`, htmlEscape(info.Topic)))
	}

	buf.WriteString(fmt.Sprintf(`<p>Members: %d</p>`, info.MemberCount))
	buf.WriteString(fmt.Sprintf(`<p>Unread: %d</p>`, info.UnreadCount))
	buf.WriteString(fmt.Sprintf(`<p>Room ID: <code>%s</code></p>`, htmlEscape(string(info.ID))))

	if info.AvatarURL != "" {
		buf.WriteString(fmt.Sprintf(`<p>Avatar: <a href="%s">%s</a></p>`, htmlEscape(info.AvatarURL), htmlEscape(info.AvatarURL)))
	}

	buf.WriteString(`</div>`)
	return buf.String()
}

func (r *HTMLRenderer) renderEvent(buf *strings.Builder, evt *EventInfo) {
	cls := msgTypeClass(evt.MsgType)
	senderHTML := htmlSender(evt.Sender, evt.SenderName)

	var timeSpan string
	if ts := htmlTime(evt.Timestamp); ts != "" {
		timeSpan = fmt.Sprintf(` <span class="time">%s</span>`, ts)
	}

	var bodyHTML string
	switch evt.MsgType {
	case "m.image", "m.file", "m.audio", "m.video":
		emoji := htmlMediaEmoji(evt.MsgType)
		u := evt.URL
		if u != "" {
			bodyHTML = fmt.Sprintf(`%s<a href="%s">%s</a>`, emoji, htmlEscape(u), htmlEscape(evt.Body))
		} else {
			bodyHTML = emoji + htmlEscape(evt.Body)
		}
	default:
		bodyHTML = htmlAutoLink(evt.Body)
	}

	buf.WriteString(fmt.Sprintf(`<div class="msg %s">%s%s<div class="body">%s</div></div>`+"\n",
		cls, senderHTML, timeSpan, bodyHTML))
}
