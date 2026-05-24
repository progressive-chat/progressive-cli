#pragma once

// libmatrix — C++20 Matrix client library
// 
// Build: cmake -B build -S . && cmake --build build
// Install: cmake --install build
// Use:     #include <libmatrix/matrix/client.hpp>
// Link:    pkg-config --libs libmatrix
//
// Example:
//   matrix::Client client;
//   client.setHomeserverURL("https://matrix.org");
//   auto creds = client.loginPassword("@user:matrix.org", "password");
//   client.startSync([](const matrix::Event& ev) { ... });

// Core Matrix client
#include "matrix/client.hpp"
#include "matrix/events.hpp"
#include "matrix/auth.hpp"
#include "matrix/error.hpp"

// E2EE (if available)
#ifdef MATRIXCLI_HAS_E2EE
#include "e2ee/crypto.hpp"
#include "e2ee/olm.hpp"
#include "e2ee/megolm.hpp"
#endif

// Storage
#include "database/db.hpp"

// Utilities
#include "util/logger.hpp"
#include "util/string_utils.hpp"
