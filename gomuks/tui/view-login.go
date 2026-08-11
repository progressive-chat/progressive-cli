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

package tui

import (
	"context"
	"math"

	"github.com/gdamore/tcell/v2"
	"github.com/mattn/go-runewidth"
	"go.mau.fi/mauview"

	"go.mau.fi/gomuks/pkg/rpc"
	"go.mau.fi/gomuks/pkg/rpc/client"
	"go.mau.fi/gomuks/tui/debug"
	"go.mau.fi/gomuks/tui/widget"
)

const (
	ConnTypeDirect     = "Direct"
	ConnTypeTor        = "Tor"
	ConnTypeI2P        = "I2P"
	ConnTypeYggdrasil  = "Yggdrasil"
	ConnTypeCustom     = "Custom"
)

var connTypes = []string{ConnTypeDirect, ConnTypeTor, ConnTypeI2P, ConnTypeYggdrasil, ConnTypeCustom}

type LoginView struct {
	*mauview.Form

	container *mauview.Centerer

	serverLabel    *mauview.TextField
	usernameLabel  *mauview.TextField
	passwordLabel  *mauview.TextField
	connTypeLabel  *mauview.TextField
	yggLabel       *mauview.TextField

	server    *mauview.InputField
	username  *mauview.InputField
	password  *mauview.InputField
	yggAddr   *mauview.InputField
	error     *mauview.TextView

	connTypeBtn   *mauview.Button
	proxyTypeBtn  *mauview.Button

	loginButton *mauview.Button
	quitButton  *mauview.Button

	connTypeIdx  int
	proxyTypeIdx int

	proxySettings *widget.ProxySettings

	loading bool

	parent *GomuksTUI
}

func (ui *GomuksTUI) NewLoginView() mauview.Component {
	view := &LoginView{
		Form: mauview.NewForm(),

		serverLabel:    mauview.NewTextField().SetText("Backend"),
		usernameLabel:  mauview.NewTextField().SetText("Username"),
		passwordLabel:  mauview.NewTextField().SetText("Password"),
		connTypeLabel:  mauview.NewTextField().SetText("Network"),
		yggLabel:       mauview.NewTextField().SetText("Yggdrasil"),

		server:    mauview.NewInputField(),
		username:  mauview.NewInputField(),
		password:  mauview.NewInputField(),
		yggAddr:   mauview.NewInputField(),

		proxySettings: widget.NewProxySettings(),

		loginButton: mauview.NewButton("Login"),
		quitButton:  mauview.NewButton("Quit"),

		parent: ui,
	}

	view.server.SetPlaceholder("http://localhost:29325").SetText(view.parent.Config.Server).SetTextColor(tcell.ColorWhite)
	view.username.SetPlaceholder("username").SetText(view.parent.Config.Username).SetTextColor(tcell.ColorWhite)
	view.password.SetPlaceholder("correct horse battery staple").SetMaskCharacter('*').SetTextColor(tcell.ColorWhite)
	view.yggAddr.SetPlaceholder("[200::1] or node.ygg").SetTextColor(tcell.ColorWhite)

	view.connTypeBtn = mauview.NewButton(view.getConnTypeLabel()).SetBackgroundColor(tcell.ColorDarkCyan).SetForegroundColor(tcell.ColorWhite).SetFocusedForegroundColor(tcell.ColorWhite)
	view.connTypeBtn.SetOnClick(view.cycleConnType)

	view.proxyTypeBtn = mauview.NewButton("SOCKS5").SetBackgroundColor(tcell.ColorDarkCyan).SetForegroundColor(tcell.ColorWhite).SetFocusedForegroundColor(tcell.ColorWhite)
	view.proxyTypeBtn.SetOnClick(view.cycleProxyType)

	view.quitButton.
		SetOnClick(func() { ui.Finish() }).
		SetBackgroundColor(tcell.ColorDarkCyan).
		SetForegroundColor(tcell.ColorWhite).
		SetFocusedForegroundColor(tcell.ColorWhite)
	view.loginButton.
		SetOnClick(view.Login).
		SetBackgroundColor(tcell.ColorDarkCyan).
		SetForegroundColor(tcell.ColorWhite).
		SetFocusedForegroundColor(tcell.ColorWhite)

	view.
		SetColumns([]int{1, 12, 1, 30, 1}).
		SetRows([]int{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1})
	view.
		AddFormItem(view.server, 3, 1, 1, 1).
		AddFormItem(view.username, 3, 3, 1, 1).
		AddFormItem(view.password, 3, 5, 1, 1).
		AddFormItem(view.connTypeBtn, 3, 7, 1, 1).
		AddFormItem(view.loginButton, 1, 17, 3, 1).
		AddFormItem(view.quitButton, 1, 19, 3, 1).
		AddComponent(view.serverLabel, 1, 1, 1, 1).
		AddComponent(view.usernameLabel, 1, 3, 1, 1).
		AddComponent(view.passwordLabel, 1, 5, 1, 1).
		AddComponent(view.connTypeLabel, 1, 7, 1, 1)
	view.FocusNextItem()
	ui.LoginView = view

	view.container = mauview.Center(mauview.NewBox(view).SetTitle("Log in to gomuks"), 47, 23)
	view.container.SetAlwaysFocusChild(true)
	return view.container
}

func (view *LoginView) getConnTypeLabel() string {
	return connTypes[view.connTypeIdx]
}

func (view *LoginView) cycleConnType() {
	view.connTypeIdx = (view.connTypeIdx + 1) % len(connTypes)
	view.connTypeBtn.SetText(view.getConnTypeLabel())
	view.updateProxyVisibility()
	view.parent.Render()
}

func (view *LoginView) cycleProxyType() {
	view.proxyTypeIdx = (view.proxyTypeIdx + 1) % 2
	if view.proxyTypeIdx == 0 {
		view.proxyTypeBtn.SetText("SOCKS5")
		view.proxySettings.SetProxyType(widget.ProxyTypeSOCKS5)
	} else {
		view.proxyTypeBtn.SetText("HTTP")
		view.proxySettings.SetProxyType(widget.ProxyTypeHTTP)
	}
	view.parent.Render()
}

func (view *LoginView) updateProxyVisibility() {
	connType := connTypes[view.connTypeIdx]

	view.RemoveComponent(view.yggLabel)
	view.RemoveComponent(view.yggAddr)
	view.RemoveFormItem(view.proxyTypeBtn)
	view.proxySettings.SetVisible(false)

	switch connType {
	case ConnTypeYggdrasil:
		view.AddComponent(view.yggLabel, 1, 9, 1, 1)
		view.AddFormItem(view.yggAddr, 3, 9, 1, 1)
	case ConnTypeCustom:
		view.AddFormItem(view.proxyTypeBtn, 3, 9, 1, 1)
		view.proxySettings.SetVisible(true)
	}
}

func (view *LoginView) Error(err string) {
	if len(err) == 0 && view.error != nil {
		debug.Print("Hiding error")
		view.RemoveComponent(view.error)
		view.container.SetHeight(23)
		view.SetRows([]int{1, 1, 1, 1, 1, 1, 1, 1, 1})
		view.error = nil
	} else if len(err) > 0 {
		debug.Print("Showing error", err)
		if view.error == nil {
			view.error = mauview.NewTextView().SetTextColor(tcell.ColorRed)
			view.AddComponent(view.error, 1, 21, 3, 1)
		}
		view.error.SetText(err)
		errorHeight := int(math.Ceil(float64(runewidth.StringWidth(err))/45)) + 1
		view.container.SetHeight(24 + errorHeight)
		view.SetRow(21, errorHeight)
	}

	view.parent.Render()
}

func (view *LoginView) actuallyLogin(server, username, password string) {
	debug.Printf("Logging into %s as %s [%s]...", server, username, connTypes[view.connTypeIdx])
	view.parent.Config.Server = server

	view.parent.ProxyConfig = view.buildProxyConfig()
	switch connTypes[view.connTypeIdx] {
	case ConnTypeYggdrasil:
		yggAddr := view.yggAddr.GetText()
		if yggAddr != "" {
			server = RewriteToYggdrasil(server, yggAddr)
			debug.Printf("Rewritten server for Yggdrasil: %s", server)
		}
	}

	var err error
	view.parent.gmx, err = client.NewGomuksClient(server)
	if err != nil {
		view.Error(err.Error())
		debug.Print("Init error:", err)
	} else if err = view.parent.gmx.GomuksAPI.(*rpc.GomuksRPC).Authenticate(context.TODO(), username, password); err != nil {
		view.Error(err.Error())
		debug.Print("Auth error:", err)
	} else {
		view.parent.Config.Username = username
		view.parent.Config.Password = password
		view.parent.Config.ConnectionType = connTypes[view.connTypeIdx]
		view.parent.Config.Save()
		view.parent.Connect()
		view.parent.SetView(ViewMain)
	}
}

func (view *LoginView) buildProxyConfig() widget.ProxyConfig {
	connType := connTypes[view.connTypeIdx]
	switch connType {
	case ConnTypeTor:
		return widget.ProxyConfig{
			ProxyType: widget.ProxyTypeSOCKS5,
			Host:      "127.0.0.1",
			Port:      9050,
		}
	case ConnTypeI2P:
		return widget.ProxyConfig{
			ProxyType: widget.ProxyTypeHTTP,
			Host:      "127.0.0.1",
			Port:      4444,
		}
	case ConnTypeCustom:
		return view.proxySettings.GetConfig()
	default:
		return widget.ProxyConfig{}
	}
}

func (view *LoginView) Login() {
	if view.loading {
		return
	}
	serverAddr := view.server.GetText()
	mxid := view.username.GetText()
	password := view.password.GetText()

	view.loading = true
	view.loginButton.SetText("Logging in...")
	go view.actuallyLogin(serverAddr, mxid, password)
}

func (view *LoginView) Draw(screen mauview.Screen) {
	view.Form.Draw(screen)

	if view.proxySettings.Visible {
		proxyX := 15
		proxyY := 11
		proxyScreen := mauview.NewProxyScreen(screen, proxyX, proxyY, 40, 6)
		view.proxySettings.Draw(proxyScreen)
	}
}

func (view *LoginView) OnKeyEvent(event mauview.KeyEvent) bool {
	if view.proxySettings.Visible {
		if view.proxySettings.OnKeyEvent(event) {
			return true
		}
	}
	return view.Form.OnKeyEvent(event)
}

func (view *LoginView) OnMouseEvent(event mauview.MouseEvent) bool {
	if view.proxySettings.Visible {
		if view.proxySettings.OnMouseEvent(event) {
			return true
		}
	}
	return view.Form.OnMouseEvent(event)
}
