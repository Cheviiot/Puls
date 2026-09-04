package main

import (
	"testing"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestParseMeasurementCommands(t *testing.T) {
	tests := []struct {
		args        []string
		service     service.ServiceID
		duration    time.Duration
		connections int
		only        phaseSelection
	}{
		{nil, "", 10 * time.Second, 0, phaseAll},
		{[]string{"yandex"}, service.Yandex, 10 * time.Second, 0, phaseAll},
		{[]string{"speedtest", "--profile", "quick", "--connections", "4"}, service.Speedtest, 5 * time.Second, 4, phaseAll},
		{[]string{"all", "--duration", "12", "--only", "ping"}, service.All, 12 * time.Second, 0, phasePing},
	}
	for _, test := range tests {
		cfg, err := parseConfig(test.args)
		if err != nil {
			t.Fatalf("parseConfig(%v): %v", test.args, err)
		}
		if cfg.Service != test.service || cfg.Duration != test.duration || cfg.Connections != test.connections || cfg.Only != test.only {
			t.Errorf("parseConfig(%v) = %+v", test.args, cfg)
		}
	}
}

func TestParseIPCommands(t *testing.T) {
	for _, test := range []struct {
		args     []string
		service  service.ServiceID
		explicit bool
	}{
		{[]string{"ip"}, "", false},
		{[]string{"ip", "speedtest"}, service.Speedtest, true},
		{[]string{"ip", "yandex", "--json"}, service.Yandex, true},
	} {
		cfg, err := parseConfig(test.args)
		if err != nil {
			t.Fatalf("parseConfig(%v): %v", test.args, err)
		}
		if cfg.Command != commandIP || cfg.Service != test.service || cfg.ServiceExplicit != test.explicit {
			t.Errorf("parseConfig(%v) = %+v", test.args, cfg)
		}
	}
}

func TestParseHelpAndVersion(t *testing.T) {
	for _, args := range [][]string{{"help"}, {"--help"}, {"-h"}} {
		cfg, err := parseConfig(args)
		if err != nil || cfg.Command != commandHelp {
			t.Errorf("parseConfig(%v) = (%+v, %v), want help", args, cfg, err)
		}
	}
	for _, args := range [][]string{{"version"}, {"--version"}} {
		cfg, err := parseConfig(args)
		if err != nil || cfg.Command != commandVersion {
			t.Errorf("parseConfig(%v) = (%+v, %v), want version", args, cfg, err)
		}
	}
}

func TestParseRejectsRemovedAndInvalidFlags(t *testing.T) {
	cases := [][]string{
		{"--ip"},
		{"ip", "all"},
		{"--duration", "2"},
		{"--duration", "61"},
		{"--connections", "17"},
		{"--profile", "slow"},
		{"--only", "latency"},
		{"yandex", "--server", "example.com"},
		{"all", "--server", "example.com"},
		{"unknown"},
	}
	for _, args := range cases {
		if _, err := parseConfig(args); err == nil {
			t.Errorf("parseConfig(%v) succeeded, want usage error", args)
		}
	}
}
