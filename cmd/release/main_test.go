package main

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRunHelp(t *testing.T) {
	if err := run([]string{"--help"}); err != nil {
		t.Fatalf("run(--help) = %v", err)
	}
}

func TestSafeVersion(t *testing.T) {
	for _, value := range []string{"dev", "1.0.0", "v2.1.0-rc.1", "build_42"} {
		if !safeVersion(value) {
			t.Errorf("safeVersion(%q) = false", value)
		}
	}
	for _, value := range []string{"", ".", "..", "1/2", "версия", "v1 beta"} {
		if safeVersion(value) {
			t.Errorf("safeVersion(%q) = true", value)
		}
	}
}

func TestParseTargets(t *testing.T) {
	supported := map[string]struct{}{"linux/amd64": {}, "windows/arm64": {}}
	targets, err := parseTargets(" linux/amd64,WINDOWS/arm64,linux/amd64 ", supported)
	if err != nil {
		t.Fatal(err)
	}
	if len(targets) != 2 || targets[0] != (target{OS: "linux", Arch: "amd64"}) || targets[1] != (target{OS: "windows", Arch: "arm64"}) {
		t.Fatalf("parseTargets() = %+v", targets)
	}
	for _, value := range []string{"", "linux", "plan9/mips"} {
		if _, err := parseTargets(value, supported); err == nil {
			t.Errorf("parseTargets(%q) returned no error", value)
		}
	}
}

func TestBuildEnvironmentReplacesTargetVariables(t *testing.T) {
	environment := buildEnvironment([]string{"PATH=/bin", "GOOS=old", "GOARCH=old", "CGO_ENABLED=1"}, target{OS: "linux", Arch: "arm64"}, buildModeCLI)
	joined := strings.Join(environment, "\n")
	for _, want := range []string{"PATH=/bin", "GOOS=linux", "GOARCH=arm64", "CGO_ENABLED=0"} {
		if !strings.Contains(joined, want) {
			t.Errorf("environment does not contain %q: %v", want, environment)
		}
	}
	for _, unwanted := range []string{"GOOS=old", "GOARCH=old", "CGO_ENABLED=1"} {
		if strings.Contains(joined, unwanted) {
			t.Errorf("environment still contains %q: %v", unwanted, environment)
		}
	}
}

func TestGUIBuildEnvironmentEnablesCGO(t *testing.T) {
	environment := buildEnvironment(nil, target{OS: "linux", Arch: "amd64"}, buildModeGUI)
	if !strings.Contains(strings.Join(environment, "\n"), "CGO_ENABLED=1") {
		t.Fatalf("GUI environment = %v", environment)
	}
	if err := validateBuildTarget(buildModeGUI, target{OS: "windows", Arch: "arm64"}); err == nil {
		t.Fatal("windows/arm64 GUI target was accepted")
	}
}

func TestReleaseArchiveFilesIncludePublicDocumentation(t *testing.T) {
	root := t.TempDir()
	binary := filepath.Join(root, "puls")
	if err := os.WriteFile(binary, []byte("binary"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, document := range releaseDocuments {
		path := filepath.Join(root, filepath.FromSlash(document))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(path, []byte(document), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	files, err := releaseArchiveFiles(root, "Puls_test", "puls", binary, false)
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != len(releaseDocuments)+1 {
		t.Fatalf("release files = %d, want %d", len(files), len(releaseDocuments)+1)
	}
	for index, document := range releaseDocuments {
		want := filepath.ToSlash(filepath.Join("Puls_test", filepath.FromSlash(document)))
		if files[index+1].Name != want || files[index+1].Mode != 0o644 {
			t.Fatalf("release document %d = %+v, want name %q and mode 0644", index, files[index+1], want)
		}
	}

	missing := filepath.Join(root, filepath.FromSlash(releaseDocuments[len(releaseDocuments)-1]))
	if err := os.Remove(missing); err != nil {
		t.Fatal(err)
	}
	if _, err := releaseArchiveFiles(root, "Puls_test", "puls", binary, false); err == nil {
		t.Fatal("releaseArchiveFiles accepted missing public documentation")
	}
}

func TestGUIArchiveIncludesApplicationIcons(t *testing.T) {
	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	binary := filepath.Join(t.TempDir(), "puls")
	if err := os.WriteFile(binary, []byte("binary"), 0o755); err != nil {
		t.Fatal(err)
	}
	files, err := releaseArchiveFiles(root, "Puls_test", "puls", binary, true)
	if err != nil {
		t.Fatal(err)
	}
	names := make(map[string]bool, len(files))
	for _, file := range files {
		names[file.Name] = true
	}
	for _, name := range []string{"Puls_test/assets/Icon.png", "Puls_test/assets/Icon.svg", "Puls_test/assets/Icon.ico"} {
		if !names[name] {
			t.Errorf("GUI archive is missing %s", name)
		}
	}
}

func TestCollectReleaseArtifactsMarksCapabilitiesAndAndroid(t *testing.T) {
	directory := t.TempDir()
	for _, name := range []string{
		"Puls_0.3.0_linux_amd64.tar.gz",
		"Puls_0.3.0_windows_arm64.zip",
		"Puls_0.3.0_android.apk",
	} {
		if err := os.WriteFile(filepath.Join(directory, name), []byte(name), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	artifacts, err := collectReleaseArtifacts(directory, "0.3.0", []target{{OS: "linux", Arch: "amd64"}, {OS: "windows", Arch: "arm64"}})
	if err != nil {
		t.Fatal(err)
	}
	if len(artifacts) != 3 {
		t.Fatalf("artifacts = %+v", artifacts)
	}
	if strings.Join(artifacts[0].Capabilities, ",") != "cli,gui" || strings.Join(artifacts[1].Capabilities, ",") != "cli" {
		t.Fatalf("desktop capabilities = %+v", artifacts)
	}
	if artifacts[2].Target != (target{OS: "android", Arch: "universal"}) || artifacts[2].Kind != "apk" {
		t.Fatalf("android artifact = %+v", artifacts[2])
	}
}

func TestCopyReleaseInstallers(t *testing.T) {
	root := t.TempDir()
	output := filepath.Join(root, "dist")
	if err := os.Mkdir(output, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, installer := range releaseInstallers {
		source := filepath.Join(root, filepath.FromSlash(installer.Source))
		if err := os.MkdirAll(filepath.Dir(source), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(source, []byte(installer.Name), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	artifacts, err := copyReleaseInstallers(root, output)
	if err != nil {
		t.Fatal(err)
	}
	if len(artifacts) != len(releaseInstallers) {
		t.Fatalf("installer artifacts = %d, want %d", len(artifacts), len(releaseInstallers))
	}
	for index, installer := range releaseInstallers {
		path := filepath.Join(output, installer.Name)
		content, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if string(content) != installer.Name || artifacts[index].Name != installer.Name {
			t.Fatalf("installer %d = %q, artifact = %+v", index, content, artifacts[index])
		}
		info, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if info.Mode().Perm() != installer.Mode {
			t.Fatalf("mode %s = %o, want %o", installer.Name, info.Mode().Perm(), installer.Mode)
		}
		if got, err := fileDigest(path); err != nil || got != artifacts[index].Digest {
			t.Fatalf("digest %s = %x, %v", installer.Name, got, err)
		}
	}

	if err := os.Remove(filepath.Join(root, filepath.FromSlash(releaseInstallers[0].Source))); err != nil {
		t.Fatal(err)
	}
	if _, err := copyReleaseInstallers(root, output); err == nil {
		t.Fatal("copyReleaseInstallers accepted a missing installer")
	}
}

func TestArchivesContainBinaryAndReadme(t *testing.T) {
	directory := t.TempDir()
	binary := filepath.Join(directory, "puls")
	readme := filepath.Join(directory, "README.md")
	if err := os.WriteFile(binary, []byte("binary"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(readme, []byte("documentation"), 0o644); err != nil {
		t.Fatal(err)
	}
	files := []archiveFile{
		{Name: "Puls_test/puls", Path: binary, Mode: 0o755},
		{Name: "Puls_test/README.md", Path: readme, Mode: 0o644},
	}

	tarPath := filepath.Join(directory, "puls.tar.gz")
	if err := writeTarGz(tarPath, files); err != nil {
		t.Fatal(err)
	}
	if contents := tarContents(t, tarPath); contents["Puls_test/puls"] != "binary" || contents["Puls_test/README.md"] != "documentation" {
		t.Fatalf("tar contents = %v", contents)
	}

	zipPath := filepath.Join(directory, "puls.zip")
	if err := writeZIP(zipPath, files); err != nil {
		t.Fatal(err)
	}
	if contents := zipContents(t, zipPath); contents["Puls_test/puls"] != "binary" || contents["Puls_test/README.md"] != "documentation" {
		t.Fatalf("zip contents = %v", contents)
	}
}

func TestArchivesAreReproducible(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "puls")
	if err := os.WriteFile(source, []byte("stable binary contents"), 0o755); err != nil {
		t.Fatal(err)
	}
	files := []archiveFile{{Name: "Puls_test/puls", Path: source, Mode: 0o755}}

	for _, format := range []struct {
		name  string
		write func(string, []archiveFile) error
	}{
		{name: "tar.gz", write: writeTarGz},
		{name: "zip", write: writeZIP},
	} {
		t.Run(format.name, func(t *testing.T) {
			firstPath := filepath.Join(directory, "first."+format.name)
			secondPath := filepath.Join(directory, "second."+format.name)
			if err := format.write(firstPath, files); err != nil {
				t.Fatal(err)
			}
			if err := os.Chtimes(source, time.Now(), time.Now()); err != nil {
				t.Fatal(err)
			}
			if err := format.write(secondPath, files); err != nil {
				t.Fatal(err)
			}
			first, err := os.ReadFile(firstPath)
			if err != nil {
				t.Fatal(err)
			}
			second, err := os.ReadFile(secondPath)
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(first, second) {
				t.Fatal("same inputs produced different archives")
			}
		})
	}
}

func TestReleaseManifestIsDeterministicAndChecksummed(t *testing.T) {
	directory := t.TempDir()
	linuxDigest := sha256.Sum256([]byte("linux archive"))
	windowsDigest := sha256.Sum256([]byte("windows archive"))
	artifacts := []artifact{
		{Name: "Puls_1.2.3_windows_arm64.zip", Digest: windowsDigest, Target: target{OS: "windows", Arch: "arm64"}, Kind: "archive", Capabilities: []string{"cli"}},
		{Name: "Puls_1.2.3_linux_amd64.tar.gz", Digest: linuxDigest, Target: target{OS: "linux", Arch: "amd64"}, Kind: "archive", Capabilities: []string{"cli", "gui"}},
	}

	manifestArtifact, err := writeReleaseManifest(directory, "1.2.3", artifacts)
	if err != nil {
		t.Fatal(err)
	}
	first, err := os.ReadFile(filepath.Join(directory, releaseManifestName))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := writeReleaseManifest(directory, "1.2.3", artifacts); err != nil {
		t.Fatal(err)
	}
	second, err := os.ReadFile(filepath.Join(directory, releaseManifestName))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(first, second) {
		t.Fatal("same artifacts produced different release manifests")
	}

	var manifest releaseManifest
	if err := json.Unmarshal(first, &manifest); err != nil {
		t.Fatal(err)
	}
	if manifest.SchemaVersion != 2 || manifest.Product != "Puls" || manifest.Version != "1.2.3" {
		t.Fatalf("manifest metadata = %+v", manifest)
	}
	if len(manifest.Assets) != 2 || manifest.Assets[0].OS != "linux" || manifest.Assets[1].OS != "windows" {
		t.Fatalf("manifest assets are not sorted by stable file name: %+v", manifest.Assets)
	}
	if manifest.Assets[0].SHA256 != fmt.Sprintf("%x", linuxDigest) {
		t.Fatalf("linux digest = %q", manifest.Assets[0].SHA256)
	}
	if manifest.Assets[0].Kind != "archive" || strings.Join(manifest.Assets[0].Capabilities, ",") != "cli,gui" {
		t.Fatalf("linux capabilities = %+v", manifest.Assets[0])
	}

	checksummed := append(append([]artifact(nil), artifacts...), manifestArtifact)
	if err := writeChecksums(directory, checksummed); err != nil {
		t.Fatal(err)
	}
	checksums, err := os.ReadFile(filepath.Join(directory, "SHA256SUMS.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(checksums), releaseManifestName) {
		t.Fatalf("checksums do not contain %s:\n%s", releaseManifestName, checksums)
	}
}

func TestReleaseManifestRejectsArtifactWithoutTarget(t *testing.T) {
	_, err := writeReleaseManifest(t.TempDir(), "1.2.3", []artifact{{Name: "broken.tar.gz"}})
	if err == nil {
		t.Fatal("writeReleaseManifest() accepted artifact without target")
	}
}

func tarContents(t *testing.T, path string) map[string]string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	gzipReader, err := gzip.NewReader(file)
	if err != nil {
		t.Fatal(err)
	}
	defer gzipReader.Close()
	reader := tar.NewReader(gzipReader)
	contents := make(map[string]string)
	for {
		header, err := reader.Next()
		if err == io.EOF {
			return contents
		}
		if err != nil {
			t.Fatal(err)
		}
		data, err := io.ReadAll(reader)
		if err != nil {
			t.Fatal(err)
		}
		contents[header.Name] = string(data)
	}
}

func zipContents(t *testing.T, path string) map[string]string {
	t.Helper()
	reader, err := zip.OpenReader(path)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	contents := make(map[string]string)
	for _, item := range reader.File {
		file, err := item.Open()
		if err != nil {
			t.Fatal(err)
		}
		data, err := io.ReadAll(file)
		file.Close()
		if err != nil {
			t.Fatal(err)
		}
		contents[item.Name] = string(data)
	}
	return contents
}
