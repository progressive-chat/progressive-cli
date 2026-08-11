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
	"math"
	"sort"
	"strings"

	"github.com/gdamore/tcell/v2"
	"github.com/mattn/go-runewidth"
	"go.mau.fi/mauview"
	"maunium.net/go/mautrix/event"

	"go.mau.fi/gomuks/pkg/rpc/store"
	"go.mau.fi/gomuks/tui/widget"
)

type MemberList struct {
	list roomMemberList
}

func NewMemberList() *MemberList {
	return &MemberList{}
}

type memberListItem struct {
	*store.AutocompleteMemberEntry
	PowerLevel int
	Sigil      rune
	Color      tcell.Color
}

type roomMemberList []*memberListItem

func (rml roomMemberList) Len() int {
	return len(rml)
}

func (rml roomMemberList) Less(i, j int) bool {
	if rml[i].PowerLevel != rml[j].PowerLevel {
		return rml[i].PowerLevel > rml[j].PowerLevel
	}
	return strings.Compare(strings.ToLower(rml[i].Displayname), strings.ToLower(rml[j].Displayname)) < 0
}

func (rml roomMemberList) Swap(i, j int) {
	rml[i], rml[j] = rml[j], rml[i]
}

func (ml *MemberList) Update(data []*store.AutocompleteMemberEntry, levels *event.PowerLevelsEventContent) *MemberList {
	ml.list = make(roomMemberList, len(data))
	highestLevel := math.MinInt32
	count := 0
	for _, level := range levels.Users {
		if level > highestLevel {
			highestLevel = level
			count = 1
		} else if level == highestLevel {
			count++
		}
	}
	for i, member := range data {
		level := levels.GetUserLevel(member.UserID)
		sigil := ' '
		if level == highestLevel && count == 1 {
			sigil = '~'
		} else if level > levels.StateDefault() {
			sigil = '&'
		} else if level >= levels.Ban() {
			sigil = '@'
		} else if level >= levels.Kick() || level >= levels.Redact() {
			sigil = '%'
		} else if level > levels.UsersDefault {
			sigil = '+'
		}
		ml.list[i] = &memberListItem{
			AutocompleteMemberEntry: member,

			PowerLevel: level,
			Sigil:      sigil,
			Color:      widget.GetHashColor(member.UserID),
		}
	}
	sort.Sort(ml.list)
	return ml
}

func (ml *MemberList) Draw(screen mauview.Screen) {
	width, _ := screen.Size()
	sigilStyle := tcell.StyleDefault.Background(tcell.ColorGreen).Foreground(tcell.ColorDefault)
	for y, member := range ml.list {
		if member.Sigil != ' ' {
			screen.SetCell(0, y, sigilStyle, member.Sigil)
		}
		if member.Membership == event.MembershipInvite {
			widget.WriteLineSimpleColor(screen, member.Displayname, 2, y, member.Color)
			screen.SetCell(1, y, tcell.StyleDefault, '(')
			if sw := runewidth.StringWidth(member.Displayname); sw+2 < width {
				screen.SetCell(sw+2, y, tcell.StyleDefault, ')')
			} else {
				screen.SetCell(width-1, y, tcell.StyleDefault, ')')
			}
		} else {
			widget.WriteLineSimpleColor(screen, member.Displayname, 1, y, member.Color)
		}
	}
}
