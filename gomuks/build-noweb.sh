#!/usr/bin/env bash
mkdir -p web/dist/
touch web/dist/empty
BINARY_NAME=gomuks MAU_VERSION_PACKAGE=go.mau.fi/gomuks/version go tool maubuild "$@"
