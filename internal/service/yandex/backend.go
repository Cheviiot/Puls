// Package yandex implements the public protocol used by
// Яндекс.Интернетометр (https://yandex.ru/internet/).
package yandex

import (
	"crypto/tls"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

const (
	defaultProbesURL       = "https://yandex.ru/internet/api/v0/get-probes?flag_ws-conn-timeout=2000"
	defaultInternetPageURL = "https://yandex.ru/internet/"
	userAgent              = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)"
	uploadChunkSize        = 2 * 1024 * 1024
	websocketChunkSize     = 64 << 10
	expectedDownloadSize   = 50 << 20
	maxDiscoveryBodySize   = 2 << 20
	maxPingBodySize        = 64 << 10
	maxDownloadBodySize    = 1 << 30
	maxUploadResponse      = 1 << 20
	maxWebsocketAckSize    = 4 << 10
	maxInternetPageSize    = 8 << 20
	clientStateMarker      = "Client.default("
)

var websocketUploadChunk = make([]byte, websocketChunkSize)

// Options configures the Yandex service backend.
type Options struct {
	Log service.LogFunc
}

// Backend implements service.Backend for Яндекс.Интернетометр.
type Backend struct {
	client          *http.Client
	dialer          *websocket.Dialer
	probesURL       string
	internetPageURL string

	mu           sync.RWMutex
	latencyURLs  []string
	downloadURLs []string
	uploadProbes []uploadProbe
	log          service.LogFunc
}

// New creates a Yandex Internetometer service.
func New(options Options) *Backend {
	transport := &http.Transport{
		TLSClientConfig:       &tls.Config{MinVersion: tls.VersionTLS12},
		MaxIdleConns:          32,
		MaxIdleConnsPerHost:   16,
		MaxConnsPerHost:       16,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: time.Second,
		DisableCompression:    true,
		ForceAttemptHTTP2:     true,
		ResponseHeaderTimeout: 10 * time.Second,
	}
	return &Backend{
		client: &http.Client{
			Transport: transport,
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		},
		dialer: &websocket.Dialer{
			HandshakeTimeout:  10 * time.Second,
			TLSClientConfig:   &tls.Config{MinVersion: tls.VersionTLS12},
			Proxy:             http.ProxyFromEnvironment,
			EnableCompression: false,
		},
		probesURL:       defaultProbesURL,
		internetPageURL: defaultInternetPageURL,
		log:             options.Log,
	}
}

func (p *Backend) ID() service.ServiceID { return service.Yandex }

func (p *Backend) Capabilities() service.Capability {
	return service.CapPing | service.CapDownload | service.CapUpload
}
