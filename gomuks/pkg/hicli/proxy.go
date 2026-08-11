// Copyright (c) 2025 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package hicli

import (
	"fmt"
	"net"
	"net/http"
	"net/url"
	"time"

	"golang.org/x/net/proxy"
)

type ProxyType int

const (
	ProxyNone ProxyType = iota
	ProxyHTTP
	ProxySOCKS5
)

func (pt ProxyType) String() string {
	switch pt {
	case ProxyNone:
		return "none"
	case ProxyHTTP:
		return "http"
	case ProxySOCKS5:
		return "socks5"
	}
	return "unknown"
}

type ProxyConfig struct {
	Type     ProxyType
	Host     string
	Port     int
	Username string
	Password string
}

var (
	DefaultTorProxy = ProxyConfig{Type: ProxySOCKS5, Host: "127.0.0.1", Port: 9050}
	DefaultI2PProxy = ProxyConfig{Type: ProxyHTTP, Host: "127.0.0.1", Port: 4444}
)

func (pc ProxyConfig) IsActive() bool {
	return pc.Type != ProxyNone && pc.Host != "" && pc.Port > 0
}

func (pc ProxyConfig) Addr() string {
	if !pc.IsActive() {
		return "none (direct)"
	}
	return fmt.Sprintf("%s://%s:%d", pc.Type, pc.Host, pc.Port)
}

func NewProxyHTTPClient(cfg ProxyConfig) *http.Client {
	transport := &http.Transport{
		DialContext:           (&net.Dialer{Timeout: 10 * time.Second}).DialContext,
		ResponseHeaderTimeout: 300 * time.Second,
		ForceAttemptHTTP2:     true,
		MaxIdleConns:          5,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
		Proxy:                 http.ProxyFromEnvironment,
	}

	if cfg.Type == ProxySOCKS5 && cfg.Host != "" && cfg.Port > 0 {
		var auth *proxy.Auth
		if cfg.Username != "" {
			auth = &proxy.Auth{User: cfg.Username, Password: cfg.Password}
		}
		dialer, err := proxy.SOCKS5("tcp", fmt.Sprintf("%s:%d", cfg.Host, cfg.Port), auth, proxy.Direct)
		if err == nil {
			transport.DialContext = nil
			transport.Dial = dialer.Dial
			transport.Proxy = nil
		}
	} else if cfg.Type == ProxyHTTP && cfg.Host != "" && cfg.Port > 0 {
		transport.Proxy = http.ProxyURL(&url.URL{
			Scheme: "http",
			Host:   fmt.Sprintf("%s:%d", cfg.Host, cfg.Port),
		})
	}

	return &http.Client{
		Transport: transport,
		Timeout:   300 * time.Second,
	}
}
