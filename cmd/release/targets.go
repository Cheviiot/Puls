package main

import (
	"errors"
	"fmt"
	"os/exec"
	"strings"
)

type target struct {
	OS   string
	Arch string
}

func supportedTargets(root string) (map[string]struct{}, error) {
	command := exec.Command("go", "tool", "dist", "list")
	command.Dir = root
	output, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("получение списка целей Go: %w", err)
	}
	result := make(map[string]struct{})
	for _, item := range strings.Fields(string(output)) {
		result[item] = struct{}{}
	}
	return result, nil
}

func parseTargets(value string, supported map[string]struct{}) ([]target, error) {
	items := strings.Split(value, ",")
	result := make([]target, 0, len(items))
	seen := make(map[string]struct{}, len(items))
	for _, item := range items {
		item = strings.ToLower(strings.TrimSpace(item))
		parts := strings.Split(item, "/")
		if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
			return nil, fmt.Errorf("неверная цель %q: ожидается формат система/архитектура", item)
		}
		if _, ok := supported[item]; !ok {
			return nil, fmt.Errorf("установленный Go не поддерживает цель %q", item)
		}
		if _, ok := seen[item]; ok {
			continue
		}
		seen[item] = struct{}{}
		result = append(result, target{OS: parts[0], Arch: parts[1]})
	}
	if len(result) == 0 {
		return nil, errors.New("список целей не может быть пустым")
	}
	return result, nil
}

func buildEnvironment(current []string, buildTarget target) []string {
	result := make([]string, 0, len(current)+3)
	for _, variable := range current {
		if strings.HasPrefix(variable, "GOOS=") || strings.HasPrefix(variable, "GOARCH=") || strings.HasPrefix(variable, "CGO_ENABLED=") {
			continue
		}
		result = append(result, variable)
	}
	return append(result, "GOOS="+buildTarget.OS, "GOARCH="+buildTarget.Arch, "CGO_ENABLED=0")
}
