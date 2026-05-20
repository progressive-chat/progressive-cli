// gomuks - A terminal Matrix client written in Go.
// Copyright (C) 2025 Tulir Asokan
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

package widget

import (
	"fmt"
	"time"

	"github.com/gdamore/tcell/v2"
	"go.mau.fi/mauview"
)

type ConnectionState int

const (
	ConnStateIdle ConnectionState = iota
	ConnStateConnecting
	ConnStateConnected
	ConnStateError
)

func (s ConnectionState) String() string {
	switch s {
	case ConnStateIdle:
		return "Idle"
	case ConnStateConnecting:
		return "Connecting..."
	case ConnStateConnected:
		return "Connected"
	case ConnStateError:
		return "Error"
	default:
		return "Unknown"
	}
}

type ProxyDisplayInfo struct {
	Type     string
	Host     string
	Port     int
	Username string
	Active   bool
}

func (p ProxyDisplayInfo) ProxyAddr() string {
	if !p.Active {
		return "none (direct)"
	}
	if p.Host == "" {
		return "none (direct)"
	}
	scheme := stringsToLower(p.Type)
	addr := fmt.Sprintf("%s://%s:%d", scheme, p.Host, p.Port)
	return addr
}

func stringsToLower(s string) string {
	result := make([]byte, len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 'A' && c <= 'Z' {
			c += 32
		}
		result[i] = c
	}
	return string(result)
}

type ConnectionStatus struct {
	mauview.FocusableComponent

	State       ConnectionState
	ConnType    string
	ProxyInfo   ProxyDisplayInfo
	StatusMsg   string
	LastUpdated time.Time

	frame int
}

func NewConnectionStatus() *ConnectionStatus {
	cs := &ConnectionStatus{
		State:    ConnStateIdle,
		ConnType: "direct",
		ProxyInfo: ProxyDisplayInfo{
			Active: false,
		},
	}
	cs.FocusableComponent = &mauview.Box{}
	return cs
}

func (cs *ConnectionStatus) SetConnectionType(ct string) {
	cs.ConnType = ct
	cs.LastUpdated = time.Now()
}

func (cs *ConnectionStatus) SetState(state ConnectionState, msg string) {
	cs.State = state
	cs.StatusMsg = msg
	cs.LastUpdated = time.Now()
	if state == ConnStateConnected {
		cs.ProxyInfo.Active = true
	}
}

func (cs *ConnectionStatus) SetProxyInfo(info ProxyDisplayInfo) {
	cs.ProxyInfo = info
	cs.LastUpdated = time.Now()
}

func (cs *ConnectionStatus) SetError(msg string) {
	cs.State = ConnStateError
	cs.StatusMsg = msg
	cs.LastUpdated = time.Now()
}

var connStateColors = map[ConnectionState]tcell.Color{
	ConnStateIdle:       tcell.ColorGray,
	ConnStateConnecting: tcell.ColorYellow,
	ConnStateConnected:  tcell.ColorGreen,
	ConnStateError:      tcell.ColorRed,
}

func (cs *ConnectionStatus) Draw(screen mauview.Screen) {
	width, height := screen.Size()
	if width <= 0 || height <= 0 {
		return
	}

	stateColor := connStateColors[cs.State]
	if stateColor == tcell.ColorDefault {
		stateColor = tcell.ColorWhite
	}

	line := 0

	connTypeLine := fmt.Sprintf("Network: %s", cs.ConnType)
	WriteLineSimple(screen, connTypeLine, 0, line)
	line++

	if line >= height {
		return
	}
	proxyLine := "Proxy: "
	if cs.ProxyInfo.Active && cs.ProxyInfo.Host != "" {
		proxyLine += cs.ProxyInfo.ProxyAddr()
	} else {
		proxyLine += "none (direct)"
	}
	WriteLineSimple(screen, proxyLine, 0, line)
	line++

	if line >= height {
		return
	}
	stateLine := fmt.Sprintf("Status: %s", cs.State.String())
	if cs.StatusMsg != "" {
		stateLine += fmt.Sprintf(" - %s", cs.StatusMsg)
	}
	WriteLineSimpleColor(screen, stateLine, 0, line, stateColor)
	line++

	if line >= height && cs.State == ConnStateConnecting {
		return
	}

	if line >= height {
		return
	}
	updatedLine := fmt.Sprintf("Updated: %s", cs.LastUpdated.Format("15:04:05"))
	WriteLineSimpleColor(screen, updatedLine, 0, line, tcell.ColorGray)
}

func (cs *ConnectionStatus) OnKeyEvent(event mauview.KeyEvent) bool {
	return false
}

func (cs *ConnectionStatus) OnMouseEvent(event mauview.MouseEvent) bool {
	return false
}
