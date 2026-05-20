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
	"strconv"
	"strings"

	"github.com/gdamore/tcell/v2"
	"go.mau.fi/mauview"
)

type ProxyType int

const (
	ProxyTypeHTTP ProxyType = iota
	ProxyTypeSOCKS5
)

var proxyTypeNames = []string{"HTTP", "SOCKS5"}

func (pt ProxyType) String() string {
	if pt >= 0 && int(pt) < len(proxyTypeNames) {
		return proxyTypeNames[pt]
	}
	return "Unknown"
}

type ProxyConfig struct {
	ProxyType ProxyType
	Host      string
	Port      int
	Username  string
	Password  string
}

func (pc ProxyConfig) ProxyURL() string {
	if pc.Host == "" || pc.Port == 0 {
		return ""
	}
	scheme := "http"
	if pc.ProxyType == ProxyTypeSOCKS5 {
		scheme = "socks5"
	}
	hostPort := fmt.Sprintf("%s:%d", pc.Host, pc.Port)
	if pc.Username != "" {
		userInfo := urlEncode(pc.Username)
		if pc.Password != "" {
			userInfo += ":" + urlEncode(pc.Password)
		}
		return fmt.Sprintf("%s://%s@%s", scheme, userInfo, hostPort)
	}
	return fmt.Sprintf("%s://%s", scheme, hostPort)
}

func urlEncode(s string) string {
	var buf strings.Builder
	for _, c := range s {
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~' {
			buf.WriteRune(c)
		} else {
			buf.WriteString(fmt.Sprintf("%%%02X", c))
		}
	}
	return buf.String()
}

type ProxySettings struct {
	mauview.FocusableComponent

	Config      ProxyConfig
	Visible     bool
	activeField int
	fields      []*mauview.InputField
	labels      []*mauview.TextField
}

var proxyFieldCount = 4

func NewProxySettings() *ProxySettings {
	ps := &ProxySettings{
		Config: ProxyConfig{
			ProxyType: ProxyTypeSOCKS5,
		},
		Visible:     false,
		activeField: 0,
	}

	ps.fields = make([]*mauview.InputField, proxyFieldCount)
	ps.labels = make([]*mauview.TextField, proxyFieldCount)

	ps.labels[0] = mauview.NewTextField().SetText("Proxy Host")
	ps.fields[0] = mauview.NewInputField().SetPlaceholder("127.0.0.1").SetTextColor(tcell.ColorWhite)

	ps.labels[1] = mauview.NewTextField().SetText("Proxy Port")
	ps.fields[1] = mauview.NewInputField().SetPlaceholder("9050").SetTextColor(tcell.ColorWhite)

	ps.labels[2] = mauview.NewTextField().SetText("Username")
	ps.fields[2] = mauview.NewInputField().SetPlaceholder("(optional)").SetTextColor(tcell.ColorWhite)

	ps.labels[3] = mauview.NewTextField().SetText("Password")
	ps.fields[3] = mauview.NewInputField().SetPlaceholder("(optional)").SetMaskCharacter('*').SetTextColor(tcell.ColorWhite)

	box := mauview.NewBox(nil).SetBorder(false)
	ps.FocusableComponent = box

	return ps
}

func (ps *ProxySettings) SetProxyType(pt ProxyType) {
	ps.Config.ProxyType = pt
	if pt == ProxyTypeSOCKS5 {
		if ps.fields[0].GetText() == "" {
			ps.fields[0].SetPlaceholder("127.0.0.1")
		}
		if ps.fields[1].GetText() == "" {
			ps.fields[1].SetPlaceholder("9050")
		}
	} else {
		if ps.fields[0].GetText() == "" {
			ps.fields[0].SetPlaceholder("127.0.0.1")
		}
		if ps.fields[1].GetText() == "" {
			ps.fields[1].SetPlaceholder("4444")
		}
	}
}

func (ps *ProxySettings) GetConfig() ProxyConfig {
	cfg := ProxyConfig{
		ProxyType: ps.Config.ProxyType,
		Host:      ps.fields[0].GetText(),
		Username:  ps.fields[2].GetText(),
		Password:  ps.fields[3].GetText(),
	}
	portStr := ps.fields[1].GetText()
	if portStr != "" {
		port, err := strconv.Atoi(portStr)
		if err == nil {
			cfg.Port = port
		}
	}
	return cfg
}

func (ps *ProxySettings) SetConfig(cfg ProxyConfig) {
	ps.Config = cfg
	if cfg.Host != "" {
		ps.fields[0].SetText(cfg.Host)
	}
	if cfg.Port > 0 {
		ps.fields[1].SetText(fmt.Sprintf("%d", cfg.Port))
	}
	if cfg.Username != "" {
		ps.fields[2].SetText(cfg.Username)
	}
	if cfg.Password != "" {
		ps.fields[3].SetText(cfg.Password)
	}
}

func (ps *ProxySettings) SetVisible(visible bool) {
	ps.Visible = visible
	if visible {
		ps.activeField = 0
	}
}

func (ps *ProxySettings) focusField() {
	for i, f := range ps.fields {
		if i == ps.activeField {
			f.SetBackgroundColor(mauview.Styles.ContrastBackgroundColor)
			f.SetPlaceholderTextColor(tcell.ColorGray)
		} else {
			f.SetBackgroundColor(tcell.ColorDefault)
			f.SetPlaceholderTextColor(tcell.ColorGray)
		}
	}
}

func (ps *ProxySettings) Draw(screen mauview.Screen) {
	if !ps.Visible {
		return
	}
	width, height := screen.Size()
	if width <= 0 || height <= 0 {
		return
	}

	ps.focusField()

	maxLabelWidth := 12
	for i := 0; i < proxyFieldCount && i < height; i++ {
		labelText := ps.labels[i].GetText()
		WriteLineSimple(screen, labelText, 0, i)
		inputStart := maxLabelWidth
		fieldText := ps.fields[i].GetText()
		placeholder := ps.fields[i].GetPlaceholder()
		if fieldText == "" && placeholder != "" {
			WriteLineSimpleColor(screen, placeholder, inputStart, i, tcell.ColorGray)
		} else {
			if i == ps.activeField {
				WriteLineSimpleColor(screen, fieldText, inputStart, i, tcell.ColorYellow)
			} else {
				WriteLineSimple(screen, fieldText, inputStart, i)
			}
		}
	}
}

func (ps *ProxySettings) OnKeyEvent(event mauview.KeyEvent) bool {
	if !ps.Visible {
		return false
	}
	switch event.Key() {
	case tcell.KeyTab:
		ps.activeField = (ps.activeField + 1) % proxyFieldCount
		return true
	case tcell.KeyBacktab:
		ps.activeField = (ps.activeField - 1 + proxyFieldCount) % proxyFieldCount
		return true
	case tcell.KeyUp:
		ps.activeField = (ps.activeField - 1 + proxyFieldCount) % proxyFieldCount
		return true
	case tcell.KeyDown:
		ps.activeField = (ps.activeField + 1) % proxyFieldCount
		return true
	case tcell.KeyRune:
		ch := event.Rune()
		if ch >= 32 && ch <= 126 {
			field := ps.fields[ps.activeField]
			currentText := field.GetText()
			field.SetText(currentText + string(ch))
			return true
		}
	case tcell.KeyBackspace, tcell.KeyBackspace2:
		field := ps.fields[ps.activeField]
		currentText := field.GetText()
		if len(currentText) > 0 {
			field.SetText(currentText[:len(currentText)-1])
		}
		return true
	}
	return false
}

func (ps *ProxySettings) OnMouseEvent(event mauview.MouseEvent) bool {
	return false
}

func (ps *ProxySettings) Focus() {
	if ps.FocusableComponent != nil {
		ps.FocusableComponent.Focus()
	}
}

func (ps *ProxySettings) Blur() {
	if ps.FocusableComponent != nil {
		ps.FocusableComponent.Blur()
	}
}
