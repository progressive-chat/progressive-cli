// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"context"
	"strings"
)

type Format string

const (
	FormatJSON     Format = "json"
	FormatText     Format = "text"
	FormatMarkdown Format = "markdown"
	FormatGemini   Format = "gemini"
	FormatHTML     Format = "html"
)

type Renderer interface {
	ContentType() string
	Render(ctx context.Context, data interface{}) ([]byte, error)
}

var registry = map[Format]Renderer{}

func RegisterRenderer(f Format, r Renderer) {
	registry[f] = r
}

func GetRenderer(f Format) (Renderer, bool) {
	r, ok := registry[f]
	return r, ok
}

func (f Format) MimeType() string {
	switch f {
	case FormatJSON:
		return "application/json"
	case FormatText:
		return "text/plain"
	case FormatMarkdown:
		return "text/markdown"
	case FormatGemini:
		return "text/gemini"
	case FormatHTML:
		return "text/html"
	default:
		return "text/plain"
	}
}

func DetectFormat(acceptHeader, queryParam string) Format {
	if queryParam != "" {
		return Format(queryParam)
	}
	if acceptHeader != "" {
		for _, f := range []Format{FormatJSON, FormatHTML, FormatMarkdown, FormatGemini, FormatText} {
			if strings.Contains(acceptHeader, string(f)) || strings.Contains(acceptHeader, f.MimeType()) {
				return f
			}
		}
	}
	return FormatJSON
}
