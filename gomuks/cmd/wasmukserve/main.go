package main

import (
	"errors"
	"net/http"
	"os"

	"github.com/rs/zerolog/hlog"
	"go.mau.fi/util/exerrors"
	"go.mau.fi/util/exhttp"
	"go.mau.fi/util/requestlog"
	"go.mau.fi/zeroconfig"
)

func main() {
	dir := os.Args[len(os.Args)-1]
	if info, err := os.Stat(dir); err != nil || !info.IsDir() {
		dir = "."
	}
	listenAddr := ":29398"
	log := exerrors.Must((&zeroconfig.Config{
		Writers: []zeroconfig.WriterConfig{{
			Type:   zeroconfig.WriterTypeStdout,
			Format: zeroconfig.LogFormatPrettyColored,
		}},
	}).Compile())
	handler := exhttp.ApplyMiddleware(
		http.FileServerFS(os.DirFS(dir)),
		func(next http.Handler) http.Handler {
			return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
				w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")
				next.ServeHTTP(w, r)
			})
		},
		hlog.NewHandler(*log),
		requestlog.AccessLogger(requestlog.Options{}),
	)
	log.Info().Str("listen_address", listenAddr).Msg("Starting server")
	err := (&http.Server{
		Addr:    listenAddr,
		Handler: handler,
	}).ListenAndServe()
	if err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal().Err(err).Msg("Server closed with error")
	}
}
