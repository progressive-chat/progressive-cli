// Copyright (c) 2025 Tulir Asokan
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

package hicli

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"

	"github.com/rs/zerolog"
	"maunium.net/go/mautrix"
)

type LoginFlowResult struct {
	Flows        []LoginFlow   `json:"flows"`
	SSOProviders []SSOProvider `json:"sso_providers,omitempty"`
}

type LoginFlow struct {
	Type string `json:"type"`
}

type SSOProvider struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	Icon string `json:"icon,omitempty"`
}

type WellKnownResponse struct {
	Homeserver HomeserverWellKnown `json:"m.homeserver"`
}

type HomeserverWellKnown struct {
	BaseURL string `json:"base_url"`
}

func DiscoverLoginFlows(ctx context.Context, homeserverURL string, proxyConfig ProxyConfig) (*LoginFlowResult, string, error) {
	log := zerolog.Ctx(ctx)

	client := NewProxyHTTPClient(proxyConfig)
	resolvedURL := homeserverURL

	wellKnownURL, _ := url.JoinPath(homeserverURL, "/.well-known/matrix/client")
	log.Debug().Str("url", wellKnownURL).Msg("Trying well-known discovery")
	body, wkErr := doHTTPGet(ctx, client, wellKnownURL)
	if wkErr == nil && body != nil {
		var wk WellKnownResponse
		if json.Unmarshal(body, &wk) == nil && wk.Homeserver.BaseURL != "" {
			resolvedURL = wk.Homeserver.BaseURL
			log.Debug().Str("resolved", resolvedURL).Msg("Well-known resolved homeserver")
		}
	}

	flowsURL, _ := url.JoinPath(resolvedURL, "/_matrix/client/r0/login")
	log.Debug().Str("url", flowsURL).Msg("Fetching login flows")
	body, err := doHTTPGet(ctx, client, flowsURL)
	if err != nil {
		return nil, resolvedURL, fmt.Errorf("failed to discover login flows: %w", err)
	}

	var result LoginFlowResult
	if err := json.Unmarshal(body, &result); err != nil {
		return nil, resolvedURL, fmt.Errorf("failed to parse login flows: %w", err)
	}

	return &result, resolvedURL, nil
}

func doHTTPGet(ctx context.Context, client *http.Client, urlStr string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, urlStr, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("HTTP %d", resp.StatusCode)
	}

	return io.ReadAll(resp.Body)
}

func (h *HiClient) LoginPasswordWithConfig(ctx context.Context, homeserverURL, username, password, deviceName string, proxyConfig ProxyConfig) error {
	log := zerolog.Ctx(ctx)

	parsedURL, err := url.Parse(homeserverURL)
	if err != nil {
		return fmt.Errorf("invalid homeserver URL: %w", err)
	}
	h.Client.HomeserverURL = parsedURL

	if deviceName != "" {
		InitialDeviceDisplayName = deviceName
	}

	if proxyConfig.IsActive() {
		log.Debug().Str("proxy", proxyConfig.Addr()).Msg("Using proxy for login")
		h.Client.Client = NewProxyHTTPClient(proxyConfig)
	}

	return h.Login(ctx, &mautrix.ReqLogin{
		Type: mautrix.AuthTypePassword,
		Identifier: mautrix.UserIdentifier{
			Type: mautrix.IdentifierTypeUser,
			User: username,
		},
		Password:                 password,
		InitialDeviceDisplayName: deviceName,
	})
}

func (h *HiClient) LoginToken(ctx context.Context, homeserverURL, token string, proxyConfig ProxyConfig) error {
	log := zerolog.Ctx(ctx)

	parsedURL, err := url.Parse(homeserverURL)
	if err != nil {
		return fmt.Errorf("invalid homeserver URL: %w", err)
	}
	h.Client.HomeserverURL = parsedURL

	if proxyConfig.IsActive() {
		log.Debug().Str("proxy", proxyConfig.Addr()).Msg("Using proxy for token login")
		h.Client.Client = NewProxyHTTPClient(proxyConfig)
	}

	return h.Login(ctx, &mautrix.ReqLogin{
		Type:                      mautrix.AuthTypeToken,
		Token:                     token,
		InitialDeviceDisplayName:  InitialDeviceDisplayName,
	})
}

func GetSSOLoginURL(homeserverURL, redirectURL string) (string, error) {
	u, err := url.Parse(homeserverURL)
	if err != nil {
		return "", fmt.Errorf("invalid homeserver URL: %w", err)
	}
	u = u.JoinPath("_matrix/client/r0/login/sso/redirect")
	q := u.Query()
	q.Set("redirectUrl", redirectURL)
	u.RawQuery = q.Encode()
	return u.String(), nil
}
