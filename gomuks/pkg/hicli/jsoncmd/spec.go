// Copyright (c) 2025 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package jsoncmd

import (
	"context"
	"encoding/json"
)

type EventSpec[Payload any] struct {
	Name Name
}

func (es *EventSpec[Payload]) Format(evt Payload) *Container[Payload] {
	return &Container[Payload]{
		Command: es.Name,
		Data:    evt,
	}
}

type ClientCommandSpec[Request, Response any] interface {
	Parse(response json.RawMessage) (Response, error)
	Format(payload Request, reqID int64) *Container[Request]
}

type CommandSpec[Request, Response any] struct {
	Name Name
}

var _ ClientCommandSpec[any, any] = (*CommandSpec[any, any])(nil)

func (cs *CommandSpec[Request, Response]) Parse(response json.RawMessage) (Response, error) {
	var resp Response
	if err := json.Unmarshal(response, &resp); err != nil {
		return resp, err
	}
	return resp, nil
}

func (cs *CommandSpec[Request, Response]) Format(payload Request, reqID int64) *Container[Request] {
	return &Container[Request]{
		Command:   cs.Name,
		RequestID: reqID,
		Data:      payload,
	}
}

func (cs *CommandSpec[Request, Response]) Run(data json.RawMessage, fn func(Request) (Response, error)) (any, error) {
	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		return nil, err
	}
	return fn(req)
}

func (cs *CommandSpec[Request, Response]) RunCtx(ctx context.Context, data json.RawMessage, fn func(context.Context, Request) (Response, error)) (any, error) {
	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		return nil, err
	}
	return fn(ctx, req)
}

type CommandSpecWithoutResponse[Request any] struct {
	Name Name
}

var _ ClientCommandSpec[any, *Empty] = (*CommandSpecWithoutResponse[any])(nil)

func (cswr *CommandSpecWithoutResponse[Request]) Parse(_ json.RawMessage) (*Empty, error) {
	return nil, nil
}

func (cswr *CommandSpecWithoutResponse[Request]) Format(payload Request, reqID int64) *Container[Request] {
	return &Container[Request]{
		Command:   cswr.Name,
		RequestID: reqID,
		Data:      payload,
	}
}

func (cswr *CommandSpecWithoutResponse[Request]) Run(data json.RawMessage, fn func(Request) error) (any, error) {
	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		return nil, err
	}
	return nil, fn(req)
}

func (cswr *CommandSpecWithoutResponse[Request]) RunCtx(ctx context.Context, data json.RawMessage, fn func(context.Context, Request) error) (any, error) {
	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		return nil, err
	}
	return nil, fn(ctx, req)
}

type CommandSpecWithoutRequest[Response any] struct {
	Name Name
}

var _ ClientCommandSpec[*Empty, any] = (*CommandSpecWithoutRequest[any])(nil)

func (cswr *CommandSpecWithoutRequest[Response]) Parse(response json.RawMessage) (Response, error) {
	var resp Response
	if err := json.Unmarshal(response, &resp); err != nil {
		return resp, err
	}
	return resp, nil
}

func (cswr *CommandSpecWithoutRequest[Response]) Format(_ *Empty, reqID int64) *Container[*Empty] {
	return &Container[*Empty]{
		Command:   cswr.Name,
		RequestID: reqID,
	}
}

func (cswr *CommandSpecWithoutRequest[Response]) Run(_ json.RawMessage, fn func() (Response, error)) (any, error) {
	return fn()
}

func (cswd *CommandSpecWithoutRequest[Response]) RunCtx(ctx context.Context, _ json.RawMessage, fn func(context.Context) (Response, error)) (any, error) {
	return fn(ctx)
}

type CommandSpecWithoutData struct {
	Name Name
}

var _ ClientCommandSpec[*Empty, *Empty] = (*CommandSpecWithoutData)(nil)

func (cswd *CommandSpecWithoutData) Parse(_ json.RawMessage) (*Empty, error) {
	return nil, nil
}

func (cswd *CommandSpecWithoutData) Format(_ *Empty, reqID int64) *Container[*Empty] {
	return &Container[*Empty]{
		Command:   cswd.Name,
		RequestID: reqID,
	}
}

func (cswd *CommandSpecWithoutData) Run(_ json.RawMessage, fn func() error) (any, error) {
	return nil, fn()
}

func (cswd *CommandSpecWithoutData) RunCtx(ctx context.Context, _ json.RawMessage, fn func(context.Context) error) (any, error) {
	return nil, fn(ctx)
}

type Empty struct{}

var EmptyVal = Empty{}
