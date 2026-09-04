//go:build live

package speedtestru

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestLiveSpeedtestRU(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 110*time.Second)
	defer cancel()
	p := New(Options{Server: ""})
	connection, err := p.DetectConnection(ctx)
	if err != nil || !connection.ExternalIP.IsValid() || connection.ISP == "" {
		t.Fatalf("connection=%+v err=%v", connection, err)
	}
	t.Logf("connection detected: ISP=%s via %s", connection.ISP, p.ID())
	var server service.Server
	for range 2 {
		server, err = p.SelectServer(ctx)
		if err == nil {
			break
		}
	}
	if err != nil {
		t.Fatal(err)
	}
	var ping service.PingResult
	for range 2 {
		ping, err = p.Ping(ctx)
		if err == nil {
			break
		}
	}
	if err != nil || ping.Samples != 10 || ping.ValueMs <= 0 {
		t.Fatalf("server=%+v ping=%+v err=%v", server, ping, err)
	}
	t.Logf("server=%s ping=%.2fms jitter=%.2fms", server.Name, ping.ValueMs, ping.JitterMs)
	if os.Getenv("PULS_LIVE_THROUGHPUT") != "1" {
		t.Log("set PULS_LIVE_THROUGHPUT=1 to run download/upload")
		return
	}
	cfg := service.MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}
	download, err := p.Download(ctx, cfg, nil)
	if err != nil || download.Bytes == 0 {
		t.Fatalf("download=%+v err=%v", download, err)
	}
	upload, err := p.Upload(ctx, cfg, nil)
	if err != nil || upload.Bytes == 0 {
		t.Fatalf("upload=%+v err=%v", upload, err)
	}
	t.Logf("download=%.2fMbps upload=%.2fMbps", download.Mbps, upload.Mbps)
}
