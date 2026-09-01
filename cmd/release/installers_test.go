package main

import (
	"bytes"
	"crypto/sha256"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

func TestPowerShellInstallerDownloadsVerifiesAndInstalls(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("install.ps1 is intended for Windows")
	}
	if runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64" {
		t.Skip("install.ps1 supports amd64 and arm64")
	}
	powerShell := ""
	for _, candidate := range []string{"pwsh.exe", "powershell.exe"} {
		if path, err := exec.LookPath(candidate); err == nil {
			powerShell = path
			break
		}
	}
	if powerShell == "" {
		t.Skip("PowerShell is unavailable")
	}

	const version = "1.2.3"
	archiveName := fmt.Sprintf("Puls_%s_windows_%s.zip", version, runtime.GOARCH)
	directory := t.TempDir()
	fakeBinary := filepath.Join(directory, "puls.exe")
	binaryContent := []byte("fake windows binary")
	if err := os.WriteFile(fakeBinary, binaryContent, 0o755); err != nil {
		t.Fatal(err)
	}
	archivePath := filepath.Join(directory, archiveName)
	if err := writeZIP(archivePath, []archiveFile{{
		Name: filepath.ToSlash(filepath.Join(strings.TrimSuffix(archiveName, ".zip"), "puls.exe")),
		Path: fakeBinary,
		Mode: 0o755,
	}}); err != nil {
		t.Fatal(err)
	}
	archive, err := os.ReadFile(archivePath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(archive)
	checksums := fmt.Sprintf("%x  %s\n", digest, archiveName)

	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write(archive)
		case "SHA256SUMS.txt":
			_, _ = response.Write([]byte(checksums))
		case releaseManifestName:
			response.Header().Set("Content-Type", "application/json")
			_, _ = fmt.Fprintf(response, `{"schema_version":1,"product":"Puls","version":%q}`, version)
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	installDir := filepath.Join(directory, "bin")
	command := exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-InstallDir", installDir,
		"-NoPathUpdate", "-RepositoryUrl", server.URL)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("install.ps1 failed: %v\n%s", err, output)
	}
	installed, err := os.ReadFile(filepath.Join(installDir, "puls.exe"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(installed, binaryContent) {
		t.Fatalf("installed binary = %q", installed)
	}
	command = exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-InstallDir", installDir,
		"-NoPathUpdate", "-RepositoryUrl", server.URL)
	output, err := command.CombinedOutput()
	if err != nil {
		t.Fatalf("second install.ps1 run failed: %v\n%s", err, output)
	}
	if !strings.Contains(string(output), "обновлён") {
		t.Fatalf("install.ps1 did not report an update:\n%s", output)
	}
	server.Close()

	command = exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-Uninstall",
		"-InstallDir", installDir, "-NoPathUpdate")
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("install.ps1 uninstall failed: %v\n%s", err, output)
	}
	if _, err := os.Stat(filepath.Join(installDir, "puls.exe")); !os.IsNotExist(err) {
		t.Fatalf("install.ps1 uninstall left the binary: %v", err)
	}
	directoryAtBinaryPath := filepath.Join(installDir, "puls.exe")
	if err := os.Mkdir(directoryAtBinaryPath, 0o755); err != nil {
		t.Fatal(err)
	}
	command = exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-Uninstall",
		"-InstallDir", installDir, "-NoPathUpdate")
	if output, err := command.CombinedOutput(); err == nil {
		t.Fatalf("install.ps1 removed a directory at the binary path:\n%s", output)
	}
	if info, err := os.Stat(directoryAtBinaryPath); err != nil || !info.IsDir() {
		t.Fatalf("directory at binary path was changed: %v, %v", info, err)
	}
}

func TestShellInstallerDownloadsVerifiesAndInstalls(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("install.sh is intended for Linux and macOS")
	}
	if runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64" {
		t.Skip("install.sh supports amd64 and arm64")
	}
	for _, command := range []string{"sh", "curl", "tar"} {
		if _, err := exec.LookPath(command); err != nil {
			t.Skipf("%s is unavailable: %v", command, err)
		}
	}

	const version = "1.2.3"
	archiveName := fmt.Sprintf("Puls_%s_%s_%s.tar.gz", version, runtime.GOOS, runtime.GOARCH)
	directory := t.TempDir()
	fakeBinary := filepath.Join(directory, "puls")
	if err := os.WriteFile(fakeBinary, []byte("#!/bin/sh\nprintf 'installed\\n'\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	archivePath := filepath.Join(directory, archiveName)
	if err := writeTarGz(archivePath, []archiveFile{{
		Name: filepath.ToSlash(filepath.Join(strings.TrimSuffix(archiveName, ".tar.gz"), "puls")),
		Path: fakeBinary,
		Mode: 0o755,
	}}); err != nil {
		t.Fatal(err)
	}
	archive, err := os.ReadFile(archivePath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(archive)
	checksums := fmt.Sprintf("%x  %s\n", digest, archiveName)

	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		if request.URL.Path == "/releases/latest" {
			http.Redirect(response, request, "/releases/tag/v"+version, http.StatusFound)
			return
		}
		if request.URL.Path == "/releases/tag/v"+version {
			_, _ = response.Write([]byte("release"))
			return
		}
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write(archive)
		case "SHA256SUMS.txt":
			_, _ = response.Write([]byte(checksums))
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	installDir := filepath.Join(directory, "bin")
	homeDir := filepath.Join(directory, "home")
	if err := os.Mkdir(homeDir, 0o755); err != nil {
		t.Fatal(err)
	}
	profileName := ".bashrc"
	if runtime.GOOS == "darwin" {
		profileName = ".bash_profile"
	}
	profilePath := filepath.Join(homeDir, profileName)
	initialProfile := "# user configuration"
	if err := os.WriteFile(profilePath, []byte(initialProfile), 0o640); err != nil {
		t.Fatal(err)
	}
	installerEnvironment := make([]string, 0, len(os.Environ())+3)
	for _, variable := range os.Environ() {
		if strings.HasPrefix(variable, "HOME=") || strings.HasPrefix(variable, "SHELL=") ||
			strings.HasPrefix(variable, "PULS_INSTALL_REPOSITORY_URL=") {
			continue
		}
		installerEnvironment = append(installerEnvironment, variable)
	}
	installerEnvironment = append(installerEnvironment,
		"HOME="+homeDir,
		"SHELL=/bin/bash",
		"PULS_INSTALL_REPOSITORY_URL="+server.URL,
	)
	installer := filepath.Join(root, "scripts", "install.sh")
	command := exec.Command("sh", installer, "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("install.sh failed: %v\n%s", err, output)
	}

	installed := filepath.Join(installDir, "puls")
	info, err := os.Stat(installed)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o755 {
		t.Fatalf("installed mode = %o, want 0755", info.Mode().Perm())
	}
	output, err := exec.Command(installed).CombinedOutput()
	if err != nil {
		t.Fatalf("installed binary failed: %v\n%s", err, output)
	}
	if string(output) != "installed\n" {
		t.Fatalf("installed output = %q", output)
	}
	wantProfileLine := initialProfile + fmt.Sprintf("\nexport PATH='%s':\"$PATH\" # Puls installer\n", installDir)
	profile, err := os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	if string(profile) != wantProfileLine {
		t.Fatalf("shell profile = %q, want %q", profile, wantProfileLine)
	}

	command = exec.Command("sh", installer, "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("second install.sh run failed: %v\n%s", err, output)
	} else if !strings.Contains(string(output), "обновлён") {
		t.Fatalf("install.sh did not report an update:\n%s", output)
	}
	profile, err = os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(profile), "# Puls installer") != 1 {
		t.Fatalf("install.sh duplicated PATH configuration: %q", profile)
	}
	savedBinary := installed + ".saved"
	if err := os.Rename(installed, savedBinary); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(installed, 0o755); err != nil {
		t.Fatal(err)
	}
	command = exec.Command("sh", installer, "--uninstall", "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err == nil {
		t.Fatalf("install.sh removed PATH after refusing the binary target:\n%s", output)
	}
	profile, err = os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(profile), "# Puls installer") {
		t.Fatalf("failed uninstall removed PATH configuration: %q", profile)
	}
	if err := os.Remove(installed); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(savedBinary, installed); err != nil {
		t.Fatal(err)
	}
	server.Close()

	command = exec.Command("sh", installer, "--uninstall", "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("install.sh uninstall failed: %v\n%s", err, output)
	}
	if _, err := os.Stat(installed); !os.IsNotExist(err) {
		t.Fatalf("install.sh uninstall left the binary: %v", err)
	}
	profile, err = os.ReadFile(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	if string(profile) != initialProfile+"\n" {
		t.Fatalf("install.sh uninstall changed user profile: %q", profile)
	}
	profileInfo, err := os.Stat(profilePath)
	if err != nil {
		t.Fatal(err)
	}
	if profileInfo.Mode().Perm() != 0o640 {
		t.Fatalf("shell profile mode = %o, want 0640", profileInfo.Mode().Perm())
	}
	command = exec.Command("sh", installer, "--uninstall", "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("second install.sh uninstall failed: %v\n%s", err, output)
	}
	if err := os.Mkdir(installed, 0o755); err != nil {
		t.Fatal(err)
	}
	command = exec.Command("sh", installer, "--uninstall", "--install-dir", installDir)
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err == nil {
		t.Fatalf("install.sh removed a directory at the binary path:\n%s", output)
	}
	if info, err := os.Stat(installed); err != nil || !info.IsDir() {
		t.Fatalf("directory at binary path was changed: %v, %v", info, err)
	}
}

func TestShellInstallerRejectsChecksumMismatch(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("install.sh is intended for Linux and macOS")
	}
	if runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64" {
		t.Skip("install.sh supports amd64 and arm64")
	}
	for _, command := range []string{"sh", "curl", "tar"} {
		if _, err := exec.LookPath(command); err != nil {
			t.Skipf("%s is unavailable: %v", command, err)
		}
	}

	const version = "1.2.3"
	archiveName := fmt.Sprintf("Puls_%s_%s_%s.tar.gz", version, runtime.GOOS, runtime.GOARCH)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write([]byte("not an archive"))
		case "SHA256SUMS.txt":
			_, _ = fmt.Fprintf(response, "%064d  %s\n", 0, archiveName)
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	installDir := filepath.Join(t.TempDir(), "bin")
	command := exec.Command("sh", filepath.Join(root, "scripts", "install.sh"),
		"--version", version, "--install-dir", installDir)
	command.Env = append(os.Environ(), "PULS_INSTALL_REPOSITORY_URL="+server.URL)
	output, err := command.CombinedOutput()
	if err == nil {
		t.Fatalf("install.sh accepted checksum mismatch:\n%s", output)
	}
	if !strings.Contains(string(output), "контрольная сумма архива не совпала") {
		t.Fatalf("unexpected installer error:\n%s", output)
	}
	if _, statErr := os.Stat(filepath.Join(installDir, "puls")); !os.IsNotExist(statErr) {
		t.Fatalf("installer left a binary after checksum failure: %v", statErr)
	}
}
