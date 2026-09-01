// Command release builds reproducible Puls archives for supported Go targets.
package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
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

type target struct {
	OS   string
	Arch string
}

type artifact struct {
	Name   string
	Digest [sha256.Size]byte
	Target target
}

type releaseManifest struct {
	SchemaVersion int                    `json:"schema_version"`
	Product       string                 `json:"product"`
	Version       string                 `json:"version"`
	Assets        []releaseManifestAsset `json:"assets"`
}

type releaseManifestAsset struct {
	OS     string `json:"os"`
	Arch   string `json:"arch"`
	File   string `json:"file"`
	SHA256 string `json:"sha256"`
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

func build(root, outputDir, workDir, version string, buildTarget target) (artifact, error) {
	name := fmt.Sprintf("Puls_%s_%s_%s", version, buildTarget.OS, buildTarget.Arch)
	binaryName := "puls"
	if buildTarget.OS == "windows" {
		binaryName += ".exe"
	}
	binaryPath := filepath.Join(workDir, name+"-"+binaryName)
	fmt.Printf("Сборка %-15s ", buildTarget.OS+"/"+buildTarget.Arch)
	command := exec.Command("go", "build", "-trimpath", "-ldflags", "-s -w -X main.version="+version, "-o", binaryPath, "./cmd/puls")
	command.Dir = root
	command.Env = buildEnvironment(os.Environ(), buildTarget)
	if output, err := command.CombinedOutput(); err != nil {
		fmt.Println("ошибка")
		return artifact{}, fmt.Errorf("%s/%s: %w\n%s", buildTarget.OS, buildTarget.Arch, err, strings.TrimSpace(string(output)))
	}

	extension := ".tar.gz"
	if buildTarget.OS == "windows" {
		extension = ".zip"
	}
	archiveName := name + extension
	archivePath := filepath.Join(outputDir, archiveName)
	temporaryArchive := archivePath + ".tmp"
	_ = os.Remove(temporaryArchive)
	files, err := releaseArchiveFiles(root, name, binaryName, binaryPath)
	if err != nil {
		return artifact{}, err
	}
	if buildTarget.OS == "windows" {
		err = writeZIP(temporaryArchive, files)
	} else {
		err = writeTarGz(temporaryArchive, files)
	}
	if err != nil {
		_ = os.Remove(temporaryArchive)
		fmt.Println("ошибка")
		return artifact{}, fmt.Errorf("упаковка %s: %w", archiveName, err)
	}
	if err := replaceFile(temporaryArchive, archivePath); err != nil {
		fmt.Println("ошибка")
		return artifact{}, fmt.Errorf("сохранение %s: %w", archiveName, err)
	}
	digest, err := fileDigest(archivePath)
	if err != nil {
		return artifact{}, err
	}
	fmt.Println("готово")
	return artifact{Name: archiveName, Digest: digest, Target: buildTarget}, nil
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

type archiveFile struct {
	Name string
	Path string
	Mode os.FileMode
}

func releaseArchiveFiles(root, directory, binaryName, binaryPath string) ([]archiveFile, error) {
	files := []archiveFile{{
		Name: filepath.ToSlash(filepath.Join(directory, binaryName)),
		Path: binaryPath,
		Mode: 0o755,
	}}
	for _, document := range releaseDocuments {
		path := filepath.Join(root, filepath.FromSlash(document))
		info, err := os.Stat(path)
		if err != nil {
			return nil, fmt.Errorf("проверка файла %s: %w", document, err)
		}
		if !info.Mode().IsRegular() {
			return nil, fmt.Errorf("файл релиза %s не является обычным файлом", document)
		}
		files = append(files, archiveFile{
			Name: filepath.ToSlash(filepath.Join(directory, filepath.FromSlash(document))),
			Path: path,
			Mode: 0o644,
		})
	}
	return files, nil
}

func writeTarGz(destination string, files []archiveFile) (returnErr error) {
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	defer func() {
		if err := output.Close(); returnErr == nil {
			returnErr = err
		}
	}()
	gzipWriter, err := gzip.NewWriterLevel(output, gzip.BestCompression)
	if err != nil {
		return err
	}
	gzipWriter.Header.ModTime = time.Unix(0, 0).UTC()
	tarWriter := tar.NewWriter(gzipWriter)
	for _, file := range files {
		info, err := os.Stat(file.Path)
		if err != nil {
			return err
		}
		header := &tar.Header{Name: file.Name, Mode: int64(file.Mode.Perm()), Size: info.Size(), ModTime: time.Unix(0, 0).UTC()}
		if err := tarWriter.WriteHeader(header); err != nil {
			return err
		}
		if err := copyFile(tarWriter, file.Path); err != nil {
			return err
		}
	}
	if err := tarWriter.Close(); err != nil {
		return err
	}
	return gzipWriter.Close()
}

func writeZIP(destination string, files []archiveFile) (returnErr error) {
	output, err := os.OpenFile(destination, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	defer func() {
		if err := output.Close(); returnErr == nil {
			returnErr = err
		}
	}()
	zipWriter := zip.NewWriter(output)
	for _, file := range files {
		info, err := os.Stat(file.Path)
		if err != nil {
			return err
		}
		header, err := zip.FileInfoHeader(info)
		if err != nil {
			return err
		}
		header.Name = file.Name
		header.Method = zip.Deflate
		header.SetMode(file.Mode)
		header.Modified = time.Date(1980, 1, 1, 0, 0, 0, 0, time.UTC)
		entry, err := zipWriter.CreateHeader(header)
		if err != nil {
			return err
		}
		if err := copyFile(entry, file.Path); err != nil {
			return err
		}
	}
	return zipWriter.Close()
}

func copyFile(destination io.Writer, source string) error {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	_, err = io.Copy(destination, input)
	return err
}

func copyReleaseInstallers(root, outputDir string) ([]artifact, error) {
	result := make([]artifact, 0, len(releaseInstallers))
	for _, installer := range releaseInstallers {
		source := filepath.Join(root, filepath.FromSlash(installer.Source))
		info, err := os.Stat(source)
		if err != nil {
			return nil, fmt.Errorf("проверка установщика %s: %w", installer.Source, err)
		}
		if !info.Mode().IsRegular() {
			return nil, fmt.Errorf("установщик %s не является обычным файлом", installer.Source)
		}

		destination := filepath.Join(outputDir, installer.Name)
		if filepath.Clean(source) != filepath.Clean(destination) {
			temporary := destination + ".tmp"
			output, err := os.OpenFile(temporary, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, installer.Mode)
			if err != nil {
				return nil, fmt.Errorf("создание установщика %s: %w", installer.Name, err)
			}
			copyErr := copyFile(output, source)
			closeErr := output.Close()
			if copyErr != nil {
				_ = os.Remove(temporary)
				return nil, fmt.Errorf("копирование установщика %s: %w", installer.Name, copyErr)
			}
			if closeErr != nil {
				_ = os.Remove(temporary)
				return nil, fmt.Errorf("сохранение установщика %s: %w", installer.Name, closeErr)
			}
			if err := os.Chmod(temporary, installer.Mode); err != nil {
				_ = os.Remove(temporary)
				return nil, fmt.Errorf("права установщика %s: %w", installer.Name, err)
			}
			if err := replaceFile(temporary, destination); err != nil {
				_ = os.Remove(temporary)
				return nil, fmt.Errorf("публикация установщика %s: %w", installer.Name, err)
			}
		}

		digest, err := fileDigest(destination)
		if err != nil {
			return nil, err
		}
		result = append(result, artifact{Name: installer.Name, Digest: digest})
	}
	return result, nil
}

func replaceFile(source, destination string) error {
	if err := os.Rename(source, destination); err == nil {
		return nil
	}
	if err := os.Remove(destination); err != nil && !errors.Is(err, os.ErrNotExist) {
		return err
	}
	return os.Rename(source, destination)
}

func fileDigest(path string) ([sha256.Size]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return [sha256.Size]byte{}, err
	}
	defer file.Close()
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return [sha256.Size]byte{}, err
	}
	var digest [sha256.Size]byte
	copy(digest[:], hash.Sum(nil))
	return digest, nil
}

func writeReleaseManifest(outputDir, version string, artifacts []artifact) (artifact, error) {
	ordered := append([]artifact(nil), artifacts...)
	sort.Slice(ordered, func(i, j int) bool { return ordered[i].Name < ordered[j].Name })
	assets := make([]releaseManifestAsset, 0, len(ordered))
	for _, item := range ordered {
		if item.Target.OS == "" || item.Target.Arch == "" {
			return artifact{}, fmt.Errorf("артефакт %q не содержит целевую систему и архитектуру", item.Name)
		}
		assets = append(assets, releaseManifestAsset{
			OS: item.Target.OS, Arch: item.Target.Arch, File: item.Name, SHA256: fmt.Sprintf("%x", item.Digest),
		})
	}
	payload, err := json.MarshalIndent(releaseManifest{
		SchemaVersion: 1,
		Product:       "Puls",
		Version:       version,
		Assets:        assets,
	}, "", "  ")
	if err != nil {
		return artifact{}, fmt.Errorf("создание манифеста релиза: %w", err)
	}
	payload = append(payload, '\n')
	path := filepath.Join(outputDir, releaseManifestName)
	temporary := path + ".tmp"
	if err := os.WriteFile(temporary, payload, 0o644); err != nil {
		_ = os.Remove(temporary)
		return artifact{}, fmt.Errorf("создание манифеста релиза: %w", err)
	}
	if err := replaceFile(temporary, path); err != nil {
		_ = os.Remove(temporary)
		return artifact{}, fmt.Errorf("сохранение манифеста релиза: %w", err)
	}
	digest, err := fileDigest(path)
	if err != nil {
		return artifact{}, err
	}
	return artifact{Name: releaseManifestName, Digest: digest}, nil
}

func writeChecksums(outputDir string, artifacts []artifact) error {
	sort.Slice(artifacts, func(i, j int) bool { return artifacts[i].Name < artifacts[j].Name })
	path := filepath.Join(outputDir, "SHA256SUMS.txt")
	temporary := path + ".tmp"
	file, err := os.OpenFile(temporary, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("создание файла контрольных сумм: %w", err)
	}
	for _, item := range artifacts {
		if _, err := fmt.Fprintf(file, "%x  %s\n", item.Digest, item.Name); err != nil {
			file.Close()
			_ = os.Remove(temporary)
			return err
		}
	}
	if err := file.Close(); err != nil {
		_ = os.Remove(temporary)
		return err
	}
	if err := replaceFile(temporary, path); err != nil {
		return fmt.Errorf("сохранение файла контрольных сумм: %w", err)
	}
	return nil
}
