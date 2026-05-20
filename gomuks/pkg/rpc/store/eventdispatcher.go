// Copyright (c) 2025 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package store

import (
	"sync"

	"go.mau.fi/util/exslices"
)

type EventDispatcher[T any] struct {
	lock      sync.RWMutex
	value     T
	listeners []*func(T)
}

func NewEventDispatcher[T any]() *EventDispatcher[T] {
	return &EventDispatcher[T]{}
}

func NewEventDispatcherWithValue[T any](val T) *EventDispatcher[T] {
	return &EventDispatcher[T]{value: val}
}

func (ed *EventDispatcher[T]) Emit(val T) {
	ed.lock.Lock()
	defer ed.lock.Unlock()
	ed.value = val
	for _, listener := range ed.listeners {
		(*listener)(val)
	}
}

func (ed *EventDispatcher[T]) Current() T {
	ed.lock.RLock()
	defer ed.lock.RUnlock()
	return ed.value
}

func (ed *EventDispatcher[T]) SetCurrent(val T) {
	ed.lock.Lock()
	defer ed.lock.Unlock()
	ed.value = val
}

func (ed *EventDispatcher[T]) Listen(listener func(T)) func() {
	ed.lock.Lock()
	defer ed.lock.Unlock()
	listenerPtr := &listener
	ed.listeners = append(ed.listeners, listenerPtr)
	return func() {
		ed.lock.Lock()
		defer ed.lock.Unlock()
		ed.listeners = exslices.FastDeleteItem(ed.listeners, listenerPtr)
	}
}

type MultiNotifier[Key comparable] struct {
	subscribers map[Key][]*func()
	lock        sync.RWMutex
}

func (mn *MultiNotifier[Key]) Notify(key Key) {
	mn.lock.RLock()
	defer mn.lock.RUnlock()
	for _, subscriber := range mn.subscribers[key] {
		(*subscriber)()
	}
}

func (mn *MultiNotifier[Key]) Listen(key Key, listener func()) func() {
	mn.lock.Lock()
	defer mn.lock.Unlock()
	if mn.subscribers == nil {
		mn.subscribers = make(map[Key][]*func())
	}
	mn.subscribers[key] = append(mn.subscribers[key], &listener)
	return func() {
		mn.lock.Lock()
		defer mn.lock.Unlock()
		mn.subscribers[key] = exslices.FastDeleteItem(mn.subscribers[key], &listener)
		if len(mn.subscribers[key]) == 0 {
			delete(mn.subscribers, key)
		}
	}
}
