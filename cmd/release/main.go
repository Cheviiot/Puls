// Command release builds reproducible Puls archives for supported Go targets.
package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const (
	defaultTargets      = "linux/amd64,linux/arm64,windows/amd64,windows/arm64,darwin/amd64,darwin/arm64"
	releaseManifestName = "RELEASE_MANIFEST.json"
)

var releaseDocuments = []string{
	"README.md",
	"CHANGELOG.md",
	"LICENSE",
	".github/CONTRIBUTING.md",
	".github/SECURITY.md",
	".github/SUPPORT.md",
	".github/CODE_OF_CONDUCT.md",
	"AGENTS.md",
	"docs/architecture.md",
	"docs/distribution.md",
}

var releaseInstallers = []struct {
	Source string
	Name   string
	Mode   os.FileMode
}{
	{Source: "scripts/install.sh", Name: "install.sh", Mode: 0o755},
	{Source: "scripts/install.ps1", Name: "install.ps1", Mode: 0o644},
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "Ошибка сборки:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("release", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	version := fs.String("version", "dev", "версия в бинарных файлах и именах архивов")
	output := fs.String("output", "dist", "каталог для готовых релизных файлов")
	targetList := fs.String("targets", defaultTargets, "список целей через запятую в формате система/архитектура")
	fs.Usage = func() {
		fmt.Fprintln(fs.Output(), "Сборка Puls для нескольких операционных систем")
		fmt.Fprintln(fs.Output())
		fmt.Fprintln(fs.Output(), "Использование:")
		fmt.Fprintln(fs.Output(), "  go run ./cmd/release --version 1.0.0")
		fmt.Fprintln(fs.Output(), "  go run ./cmd/release --targets linux/riscv64,freebsd/amd64")
		fmt.Fprintln(fs.Output())
		fmt.Fprintln(fs.Output(), "Параметры:")
		fmt.Fprintln(fs.Output(), "  --version <value>   версия Puls · по умолчанию dev")
		fmt.Fprintln(fs.Output(), "  --output <path>     каталог релизных файлов · по умолчанию dist")
		fmt.Fprintln(fs.Output(), "  --targets <list>    пары система/архитектура через запятую")
	}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return nil
		}
		return err
	}
	if fs.NArg() != 0 {
		return fmt.Errorf("неизвестный аргумент %q", fs.Arg(0))
	}
	if !safeVersion(*version) {
		return fmt.Errorf("версия %q содержит недопустимые символы", *version)
	}
	if strings.TrimSpace(*output) == "" {
		return errors.New("каталог результатов не может быть пустым")
	}

	root, err := findProjectRoot()
	if err != nil {
		return err
	}
	supported, err := supportedTargets(root)
	if err != nil {
		return err
	}
	targets, err := parseTargets(*targetList, supported)
	if err != nil {
		return err
	}

	outputDir := *output
	if !filepath.IsAbs(outputDir) {
		outputDir = filepath.Join(root, outputDir)
	}
	if err := os.MkdirAll(outputDir, 0o755); err != nil {
		return fmt.Errorf("создание каталога результатов: %w", err)
	}
	workDir, err := os.MkdirTemp(outputDir, ".puls-build-")
	if err != nil {
		return fmt.Errorf("создание временного каталога: %w", err)
	}
	defer os.RemoveAll(workDir)

	artifacts := make([]artifact, 0, len(targets))
	for _, buildTarget := range targets {
		built, buildErr := build(root, outputDir, workDir, *version, buildTarget)
		if buildErr != nil {
			return buildErr
		}
		artifacts = append(artifacts, built)
	}
	manifestArtifact, err := writeReleaseManifest(outputDir, *version, artifacts)
	if err != nil {
		return err
	}
	installers, err := copyReleaseInstallers(root, outputDir)
	if err != nil {
		return err
	}
	checksummed := make([]artifact, 0, len(artifacts)+len(installers)+1)
	checksummed = append(checksummed, artifacts...)
	checksummed = append(checksummed, installers...)
	checksummed = append(checksummed, manifestArtifact)
	if err := writeChecksums(outputDir, checksummed); err != nil {
		return err
	}
	fmt.Printf("Готово: %d архивов в %s\n", len(artifacts), outputDir)
	return nil
}

func safeVersion(value string) bool {
	if value == "" || value == "." || value == ".." {
		return false
	}
	for _, r := range value {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '.' || r == '_' || r == '-' {
			continue
		}
		return false
	}
	return true
}

func findProjectRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		if info, statErr := os.Stat(filepath.Join(dir, "go.mod")); statErr == nil && !info.IsDir() {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", errors.New("не найден корень проекта с файлом go.mod")
		}
		dir = parent
	}
}
