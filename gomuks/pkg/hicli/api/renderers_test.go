// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"testing"
)

func TestDetectFormat_QueryParam(t *testing.T) {
	testCases := []struct {
		name       string
		queryParam string
		expected   Format
	}{
		{"json query param", "json", FormatJSON},
		{"text query param", "text", FormatText},
		{"markdown query param", "markdown", FormatMarkdown},
		{"gemini query param", "gemini", FormatGemini},
		{"html query param", "html", FormatHTML},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			result := DetectFormat("", tc.queryParam)
			if result != tc.expected {
				t.Errorf("expected %q, got %q", tc.expected, result)
			}
		})
	}
}

func TestDetectFormat_QueryParamOverridesAccept(t *testing.T) {
	result := DetectFormat("application/json", "html")
	if result != FormatHTML {
		t.Errorf("expected %q, got %q", FormatHTML, result)
	}
}

func TestDetectFormat_AcceptHeader(t *testing.T) {
	testCases := []struct {
		name         string
		acceptHeader string
		expected     Format
	}{
		{"json accept", "application/json", FormatJSON},
		{"html accept", "text/html", FormatHTML},
		{"markdown accept", "text/markdown", FormatMarkdown},
		{"gemini accept", "text/gemini", FormatGemini},
		{"plain text accept", "text/plain", FormatText},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			result := DetectFormat(tc.acceptHeader, "")
			if result != tc.expected {
				t.Errorf("expected %q, got %q", tc.expected, result)
			}
		})
	}
}

func TestDetectFormat_Default(t *testing.T) {
	result := DetectFormat("", "")
	if result != FormatJSON {
		t.Errorf("expected %q, got %q", FormatJSON, result)
	}
}

func TestDetectFormat_AcceptHeaderShortName(t *testing.T) {
	result := DetectFormat("json", "")
	if result != FormatJSON {
		t.Errorf("expected %q, got %q", FormatJSON, result)
	}
}

func TestDetectFormat_EmptyAcceptFallsThrough(t *testing.T) {
	result := DetectFormat("", "unknown")
	if result != Format("unknown") {
		t.Errorf("expected %q, got %q", Format("unknown"), result)
	}
}
