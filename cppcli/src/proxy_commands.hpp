#pragma once

#include "core/http_client.hpp"

// Persisted Tor/I2P/SOCKS5 proxy. Single source of truth is config.json:
//   "proxy_presets": [{"name", "type", "host", "port", "user"?, "pass"?}, ...]
//   "proxy_active": "<preset name>"  ("" = disabled)
// The old flat proxy_enabled/proxy_host/... keys are migrated away on load.
//
// applyProxyFromConfig is called at every process start (main()); the other
// two helpers are shared with demo mode and the legacy serve-login path.
void applyProxyFromConfig();
bool activeProxyConfig(progressive::desktop::ProxyConfig* out);
void persistActiveProxy(const progressive::desktop::ProxyConfig& cfg);
