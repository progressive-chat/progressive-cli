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
	"fmt"
	"net"
	"net/url"
	"strings"
)

func IsYggdrasilAddress(addr string) bool {
	if len(addr) < 4 {
		return false
	}
	lower := strings.ToLower(addr)
	dot := strings.LastIndex(lower, ".ygg")
	if dot == len(lower)-4 {
		return true
	}
	if strings.HasPrefix(lower, ".ygg") {
		return true
	}
	return false
}

func IsYggdrasilIPv6(addr string) bool {
	clean := addr
	clean = strings.TrimSpace(clean)
	if len(clean) >= 2 && clean[0] == '[' {
		end := strings.Index(clean, "]")
		if end > 0 {
			clean = clean[1:end]
		}
	}
	if len(clean) < 4 {
		return false
	}
	lower := strings.ToLower(clean)
	if lower[0] == '2' && lower[1] >= '0' && lower[1] <= 'f' {
		return true
	}
	if lower[0] == '3' && lower[1] >= '0' && lower[1] <= 'f' {
		return true
	}
	return false
}

func IsYggdrasilOrOnionOrI2P(addr string) string {
	addrLower := strings.ToLower(addr)

	if strings.HasSuffix(addrLower, ".onion") {
		return "tor"
	}
	if strings.HasSuffix(addrLower, ".i2p") {
		return "i2p"
	}
	if IsYggdrasilAddress(addrLower) {
		return "yggdrasil"
	}
	if IsYggdrasilIPv6(addr) {
		return "yggdrasil"
	}
	return ""
}

func NormalizeYggAddress(addr string) string {
	result := addr
	result = strings.TrimSpace(result)
	if len(result) >= 2 && result[0] == '[' {
		end := strings.Index(result, "]")
		if end > 0 {
			result = result[1:end]
		}
	}
	return strings.ToLower(result)
}

func BuildYggHomeserverURL(yggAddress string, port int, tls bool) string {
	scheme := "http"
	if tls {
		scheme = "https"
	}
	normalized := NormalizeYggAddress(yggAddress)
	u := fmt.Sprintf("%s://[%s]", scheme, normalized)
	if port > 0 {
		defaultPort := 80
		if tls {
			defaultPort = 443
		}
		if port != defaultPort {
			u = fmt.Sprintf("%s:%d", u, port)
		}
	}
	return u
}

func RewriteToYggdrasil(homeserverURL string, yggAddress string) string {
	if yggAddress == "" {
		return ""
	}
	parsed, err := url.Parse(homeserverURL)
	if err != nil {
		return ""
	}
	tls := parsed.Scheme == "https"
	defaultPort := 80
	if tls {
		defaultPort = 443
	}
	port := defaultPort
	if parsed.Port() != "" {
		fmt.Sscanf(parsed.Port(), "%d", &port)
	}
	return BuildYggHomeserverURL(yggAddress, port, tls)
}

func ValidateYggdrasilEndpoint(addr string) error {
	clean := NormalizeYggAddress(addr)
	if clean == "" {
		return fmt.Errorf("empty yggdrasil address")
	}
	if IsYggdrasilIPv6(clean) {
		ip := net.ParseIP(clean)
		if ip == nil {
			return fmt.Errorf("invalid yggdrasil IPv6 address: %s", clean)
		}
		if ip.To4() != nil {
			return fmt.Errorf("expected IPv6 yggdrasil address, got IPv4")
		}
		return nil
	}
	if IsYggdrasilAddress(clean) {
		return nil
	}
	return fmt.Errorf("not a valid yggdrasil address: %s", clean)
}
