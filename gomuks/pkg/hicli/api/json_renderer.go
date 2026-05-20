// Copyright (c) 2026 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package api

import (
	"context"
	"encoding/json"
)

type JSONRenderer struct{}

func (r *JSONRenderer) ContentType() string {
	return "application/json"
}

func (r *JSONRenderer) Render(ctx context.Context, data interface{}) ([]byte, error) {
	return json.MarshalIndent(data, "", "  ")
}

func init() {
	RegisterRenderer(FormatJSON, &JSONRenderer{})
}
