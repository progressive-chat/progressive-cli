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

package debug

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"runtime/debug"
	"time"

	badGlobalLog "github.com/rs/zerolog/log"
)

var RecoverPrettyPanic = true
var OnRecover func()

func Printf(text string, args ...any) {
	badGlobalLog.Debug().CallerSkipFrame(1).Msgf(text, args...) // zerolog-allow-msgf
}

func Print(text ...any) {
	msg := fmt.Sprintln(text...)
	badGlobalLog.Debug().CallerSkipFrame(1).Msg(msg[:len(msg)-1])
}

func PrintStack() {
	badGlobalLog.Error().CallerSkipFrame(1).Bytes("stack", debug.Stack()).Msg("Stack trace")
}

// Recover recovers a panic, runs the OnRecover handler and either re-panics or
// shows an user-friendly message about the panic depending on whether or not
// the pretty panic mode is enabled.
func Recover() {
	if p := recover(); p != nil {
		if OnRecover != nil {
			OnRecover()
		}
		if RecoverPrettyPanic {
			PrettyPanic(p)
		} else {
			panic(p)
		}
	}
}

const Oops = ` __________
< Oh noes! >
 ‾‾‾\‾‾‾‾‾‾
     \   ^__^
      \  (XX)\_______
         (__)\       )\/\
          U  ||----W |
             ||     ||

A fatal error has occurred.

`

func PrettyPanic(panic interface{}) {
	fmt.Print(Oops)
	traceFile := fmt.Sprintf(filepath.Join(os.TempDir(), "gomuks-panic-%s.txt"), time.Now().Format("2006-01-02--15-04-05"))

	var buf bytes.Buffer
	_, _ = fmt.Fprintln(&buf, panic)
	buf.Write(debug.Stack())
	err := os.WriteFile(traceFile, buf.Bytes(), 0600)

	if err != nil {
		fmt.Println("Saving the stack trace to", traceFile, "failed:")
		fmt.Println("--------------------------------------------------------------------------------")
		fmt.Println(err)
		fmt.Println("--------------------------------------------------------------------------------")
		fmt.Println("")
		fmt.Println("You can file an issue at https://github.com/gomuks/gomuks/issues.")
		fmt.Println("Please provide the file save error (above) and the stack trace of the original error (below) when filing an issue.")
		fmt.Println("")
		fmt.Println("--------------------------------------------------------------------------------")
		fmt.Println(panic)
		debug.PrintStack()
		fmt.Println("--------------------------------------------------------------------------------")
	} else {
		fmt.Println("The stack trace has been saved to", traceFile)
		fmt.Println("")
		fmt.Println("You can file an issue at https://github.com/gomuks/gomuks/issues.")
		fmt.Println("Please provide the contents of that file when filing an issue.")
	}
	os.Exit(1)
}
