// Package speedtestru implements the first-party QMS protocol used by
// speedtest.ru. Server discovery and short-lived JWT issuance use the public
// browser API; measurements themselves go directly to the selected QMS hosts.
package speedtestru

import (
	"crypto/tls"
	"errors"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

const (
	defaultAPIBase     = "https://speedtest.ru"
	defaultBrowserKey  = "5f3287b55fbcd8076919114885f8f3f7"
	userAgent          = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)"
	uploadBlockSize    = 2 * 1024 * 1024
	maxDiscoveryBytes  = 4 << 20
	maxPageBytes       = 8 << 20
	maxJWTBytes        = 1 << 20
	maxConnectionBytes = 64 << 10
	maxISPNameBytes    = 255
	apiRequestTimeout  = 15 * time.Second
	websocketReadLimit = 64 << 10
)

var errPingOnlyFallback = errors.New("резервный список speedtest.ru поддерживает только проверку задержки")

var knownServers = []struct{ host, city string }{
	{"vladivostok.qms.ru:20000", "Владивосток"},
	{"vladivostok10.qms.ru:20000", "Владивосток"},
	{"khabarovsk1.qms.ru:20000", "Хабаровск"},
	{"blagoveshchensk.qms.ru:20000", "Благовещенск"},
	{"yuzhno-sahalinsk.qms.ru:20000", "Южно-Сахалинск"},
	{"chita.qms.ru:20000", "Чита"},
	{"ulan-ude.qms.ru:20000", "Улан-Удэ"},
	{"yakutsk.qms.ru:20000", "Якутск"},
	{"magadan.qms.ru:20000", "Магадан"},
}

type qmsServer struct {
	ID     int
	Host   string
	Name   string
	City   string
	Region string
	RTT    time.Duration
	Ping   service.PingResult
}

func (s qmsServer) wsURL() string { return "wss://" + s.Host + "/" }

func (s qmsServer) httpURL() string { return "https://" + s.Host + "/" }

type nearestResponse struct {
	Algorithm string `json:"algorithm"`
	Data      []struct {
		ID         int    `json:"id"`
		Name       string `json:"name"`
		City       string `json:"city"`
		Src        string `json:"src"`
		Source     string `json:"source"`
		Port       int    `json:"port"`
		RegionName string `json:"region_name"`
	} `json:"data"`
}

// Options configures the speedtest.ru service backend.
type Options struct {
	Server string
	Log    service.LogFunc
}

// Backend speaks QMS to either a caller-selected host or dynamically
// discovered speedtest.ru hosts.
type Backend struct {
	host string

	client  *http.Client
	dialer  *websocket.Dialer
	apiBase string

	mu           sync.RWMutex
	servers      []qmsServer
	selected     qmsServer
	browserKey   string
	jwt          string
	authMu       sync.Mutex
	keyMu        sync.Mutex
	throughput   bool
	discoveryErr error
	log          service.LogFunc
}

func New(options Options) *Backend {
	transport := &http.Transport{
		DialContext:           (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
		TLSClientConfig:       &tls.Config{MinVersion: tls.VersionTLS12},
		MaxIdleConns:          32,
		MaxIdleConnsPerHost:   16,
		MaxConnsPerHost:       16,
		DisableCompression:    true,
		ForceAttemptHTTP2:     true,
		IdleConnTimeout:       30 * time.Second,
		TLSHandshakeTimeout:   8 * time.Second,
		ExpectContinueTimeout: time.Second,
		ResponseHeaderTimeout: 12 * time.Second,
	}
	return &Backend{
		host:   options.Server,
		client: &http.Client{Transport: transport},
		dialer: &websocket.Dialer{
			HandshakeTimeout: 8 * time.Second,
			NetDialContext:   (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
			TLSClientConfig:  &tls.Config{MinVersion: tls.VersionTLS12},
		},
		apiBase:    defaultAPIBase,
		browserKey: defaultBrowserKey,
		log:        options.Log,
	}
}

func (p *Backend) ID() service.ServiceID { return service.Speedtest }

func (p *Backend) logf(format string, values ...any) {
	if p.log != nil {
		p.log(format, values...)
	}
}

func (p *Backend) Capabilities() service.Capability {
	return service.CapPing | service.CapDownload | service.CapUpload
}

func normalizeHost(host, defaultPort string) (string, error) {
	host = strings.TrimSpace(host)
	if host == "" {
		return "", errors.New("адрес сервера не может быть пустым")
	}
	if strings.ContainsAny(host, "/?#@") || strings.Contains(host, "://") {
		return "", fmt.Errorf("неверный адрес сервера %q: укажите host или host:port", host)
	}
	if ip := net.ParseIP(strings.Trim(host, "[]")); ip != nil {
		return net.JoinHostPort(ip.String(), defaultPort), nil
	}
	hostname, port, err := net.SplitHostPort(host)
	if err != nil {
		if strings.Contains(host, ":") {
			return "", fmt.Errorf("неверный адрес сервера %q: %w", host, err)
		}
		hostname, port = host, defaultPort
	}
	if hostname == "" || strings.ContainsAny(hostname, " \t\r\n") {
		return "", fmt.Errorf("неверное имя сервера %q", hostname)
	}
	portNumber, err := strconv.Atoi(port)
	if err != nil || portNumber < 1 || portNumber > 65535 {
		return "", fmt.Errorf("неверный порт сервера %q", port)
	}
	return net.JoinHostPort(hostname, strconv.Itoa(portNumber)), nil
}
