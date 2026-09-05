package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"unicode/utf8"
)

func TestPowerShellInstallerIsASCIIWithoutBOM(t *testing.T) {
	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	content, err := os.ReadFile(filepath.Join(root, "scripts", "install.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	if bytes.HasPrefix(content, []byte{0xef, 0xbb, 0xbf}) {
		t.Fatal("install.ps1 BOM breaks irm | iex in Windows PowerShell 5")
	}
	if !utf8.Valid(content) {
		t.Fatal("install.ps1 is not valid UTF-8")
	}
	for offset, value := range content {
		if value > 0x7f {
			t.Fatalf("install.ps1 byte %d is non-ASCII and will be corrupted by Windows PowerShell 5", offset)
		}
	}
}

func TestPowerShellInstallerDownloadsVerifiesAndInstalls(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("install.ps1 is intended for Windows")
	}
	if runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64" {
		t.Skip("install.ps1 supports amd64 and arm64")
	}
	powerShell := ""
	for _, candidate := range []string{"powershell.exe", "pwsh.exe"} {
		if path, err := exec.LookPath(candidate); err == nil {
			powerShell = path
			break
		}
	}
	if powerShell == "" {
		t.Skip("PowerShell is unavailable")
	}
	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	installerContent, err := os.ReadFile(filepath.Join(root, "scripts", "install.ps1"))
	if err != nil {
		t.Fatal(err)
	}
	helpCommand := exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-Help")
	helpOutput, err := helpCommand.CombinedOutput()
	if err != nil {
		t.Fatalf("direct install.ps1 help failed: %v\n%s", err, helpOutput)
	}
	if !strings.Contains(string(helpOutput), "Установка и удаление Puls") {
		t.Fatalf("direct install.ps1 help lost Russian text:\n%s", helpOutput)
	}

	const version = "1.2.3"
	archiveName := fmt.Sprintf("Puls_%s_windows_%s.zip", version, runtime.GOARCH)
	directory := t.TempDir()
	fakeBinary := filepath.Join(directory, "puls.exe")
	fakeIcon := filepath.Join(directory, "Icon.ico")
	binaryContent := []byte("fake windows binary")
	if err := os.WriteFile(fakeBinary, binaryContent, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fakeIcon, []byte("fake icon"), 0o644); err != nil {
		t.Fatal(err)
	}
	archivePath := filepath.Join(directory, archiveName)
	packageName := strings.TrimSuffix(archiveName, ".zip")
	if err := writeZIP(archivePath, []archiveFile{
		{Name: filepath.ToSlash(filepath.Join(packageName, "puls.exe")), Path: fakeBinary, Mode: 0o755},
		{Name: filepath.ToSlash(filepath.Join(packageName, "assets", "Icon.ico")), Path: fakeIcon, Mode: 0o644},
	}); err != nil {
		t.Fatal(err)
	}
	archive, err := os.ReadFile(archivePath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(archive)
	manifest := testReleaseManifestWithCapabilities(t, version, "windows", runtime.GOARCH, archiveName, digest, []string{"cli", "gui"})
	manifestDigest := sha256.Sum256(manifest)
	checksums := fmt.Sprintf("%x  %s\n%x  %s\n", digest, archiveName, manifestDigest, releaseManifestName)

	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write(archive)
		case "SHA256SUMS.txt":
			_, _ = response.Write([]byte(checksums))
		case releaseManifestName:
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
		case "install.ps1":
			response.Header().Set("Content-Type", "text/plain; charset=utf-8")
			_, _ = response.Write(installerContent)
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	installDir := filepath.Join(directory, "bin")
	shortcutDir := filepath.Join(directory, "shortcuts")
	newInstallCommand := func() *exec.Cmd {
		command := exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
			"Invoke-RestMethod -UseBasicParsing -Uri '"+server.URL+"/install.ps1' | Invoke-Expression")
		command.Env = append(os.Environ(),
			"PULS_INSTALL_DIR="+installDir,
			"PULS_INSTALL_REPOSITORY_URL="+server.URL,
			"PULS_SHORTCUT_DIR="+shortcutDir,
		)
		return command
	}
	command := newInstallCommand()
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("irm | iex install.ps1 failed: %v\n%s", err, output)
	}
	installed, err := os.ReadFile(filepath.Join(installDir, "puls.exe"))
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(installed, binaryContent) {
		t.Fatalf("installed binary = %q", installed)
	}
	if _, err := os.Stat(filepath.Join(shortcutDir, "Puls.lnk")); err != nil {
		t.Fatalf("Start Menu shortcut was not created: %v", err)
	}
	if icon, err := os.ReadFile(filepath.Join(installDir, "Puls.ico")); err != nil || string(icon) != "fake icon" {
		t.Fatalf("installed icon = %q, %v", icon, err)
	}
	command = newInstallCommand()
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
		"-InstallDir", installDir)
	command.Env = append(os.Environ(), "PULS_SHORTCUT_DIR="+shortcutDir)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("install.ps1 uninstall failed: %v\n%s", err, output)
	}
	if _, err := os.Stat(filepath.Join(installDir, "puls.exe")); !os.IsNotExist(err) {
		t.Fatalf("install.ps1 uninstall left the binary: %v", err)
	}
	if _, err := os.Stat(filepath.Join(shortcutDir, "Puls.lnk")); !os.IsNotExist(err) {
		t.Fatalf("install.ps1 uninstall left the shortcut: %v", err)
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

func TestPowerShellInstallerRejectsWrongManifestPackage(t *testing.T) {
	if runtime.GOOS != "windows" {
		t.Skip("install.ps1 is intended for Windows")
	}
	if runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64" {
		t.Skip("install.ps1 supports amd64 and arm64")
	}
	powerShell := ""
	for _, candidate := range []string{"powershell.exe", "pwsh.exe"} {
		if path, err := exec.LookPath(candidate); err == nil {
			powerShell = path
			break
		}
	}
	if powerShell == "" {
		t.Skip("PowerShell is unavailable")
	}

	const version = "1.2.3"
	wrongName := fmt.Sprintf("Puls_%s_windows_%s-wrong.zip", version, runtime.GOARCH)
	manifest := testReleaseManifest(t, version, "windows", runtime.GOARCH, wrongName, [sha256.Size]byte{})
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		if filepath.Base(request.URL.Path) == releaseManifestName {
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
			return
		}
		http.NotFound(response, request)
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	installDir := filepath.Join(t.TempDir(), "bin")
	command := exec.Command(powerShell, "-NoProfile", "-ExecutionPolicy", "Bypass",
		"-File", filepath.Join(root, "scripts", "install.ps1"), "-Version", version,
		"-InstallDir", installDir, "-NoPathUpdate", "-RepositoryUrl", server.URL)
	output, err := command.CombinedOutput()
	if err == nil {
		t.Fatalf("install.ps1 accepted a wrong manifest package:\n%s", output)
	}
	if !strings.Contains(string(output), "RELEASE_MANIFEST.json") || !strings.Contains(string(output), wrongName) {
		t.Fatalf("unexpected install.ps1 error:\n%s", output)
	}
	if _, statErr := os.Stat(filepath.Join(installDir, "puls.exe")); !os.IsNotExist(statErr) {
		t.Fatalf("installer left a binary after manifest failure: %v", statErr)
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
	manifest := testReleaseManifest(t, version, runtime.GOOS, runtime.GOARCH, archiveName, digest)
	manifestDigest := sha256.Sum256(manifest)
	checksums := fmt.Sprintf("%x  %s\n%x  %s\n", digest, archiveName, manifestDigest, releaseManifestName)

	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write(archive)
		case "SHA256SUMS.txt":
			_, _ = response.Write([]byte(checksums))
		case releaseManifestName:
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	homeDir := filepath.Join(directory, "home")
	if err := os.Mkdir(homeDir, 0o755); err != nil {
		t.Fatal(err)
	}
	installDir := filepath.Join(homeDir, ".local", "bin")
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
	command := exec.Command("sh", installer)
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

	command = exec.Command("sh", installer)
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
	command = exec.Command("sh", installer, "--uninstall")
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

	command = exec.Command("sh", installer, "--uninstall")
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
	command = exec.Command("sh", installer, "--uninstall")
	command.Env = installerEnvironment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("second install.sh uninstall failed: %v\n%s", err, output)
	}
	if err := os.Mkdir(installed, 0o755); err != nil {
		t.Fatal(err)
	}
	command = exec.Command("sh", installer, "--uninstall")
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
	zeroDigest := [sha256.Size]byte{}
	manifest := testReleaseManifest(t, version, runtime.GOOS, runtime.GOARCH, archiveName, zeroDigest)
	manifestDigest := sha256.Sum256(manifest)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write([]byte("not an archive"))
		case "SHA256SUMS.txt":
			_, _ = fmt.Fprintf(response, "%064d  %s\n%x  %s\n", 0, archiveName, manifestDigest, releaseManifestName)
		case releaseManifestName:
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
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

func TestShellInstallerManagesGUIShortcut(t *testing.T) {
	if (runtime.GOOS != "linux" && runtime.GOOS != "darwin") || (runtime.GOARCH != "amd64" && runtime.GOARCH != "arm64") {
		t.Skip("desktop shortcut integration test is Linux/macOS-only")
	}
	const version = "1.2.3"
	archiveName := fmt.Sprintf("Puls_%s_%s_%s.tar.gz", version, runtime.GOOS, runtime.GOARCH)
	directory := t.TempDir()
	binary := filepath.Join(directory, "puls")
	icon := filepath.Join(directory, "Icon.png")
	if err := os.WriteFile(binary, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(icon, []byte("icon"), 0o644); err != nil {
		t.Fatal(err)
	}
	packageName := strings.TrimSuffix(archiveName, ".tar.gz")
	archivePath := filepath.Join(directory, archiveName)
	if err := writeTarGz(archivePath, []archiveFile{
		{Name: packageName + "/puls", Path: binary, Mode: 0o755},
		{Name: packageName + "/assets/Icon.png", Path: icon, Mode: 0o644},
	}); err != nil {
		t.Fatal(err)
	}
	archive, err := os.ReadFile(archivePath)
	if err != nil {
		t.Fatal(err)
	}
	digest := sha256.Sum256(archive)
	manifest := testReleaseManifestWithCapabilities(t, version, runtime.GOOS, runtime.GOARCH, archiveName, digest, []string{"cli", "gui"})
	var manifestDocument releaseManifest
	if err := json.Unmarshal(manifest, &manifestDocument); err != nil {
		t.Fatal(err)
	}
	manifestDocument.Assets = append([]releaseManifestAsset{{
		OS: "darwin", Arch: "amd64", File: "Puls_1.2.3_darwin_amd64.tar.gz",
		SHA256: strings.Repeat("0", 64), Kind: "archive", Capabilities: []string{"cli", "gui"},
	}}, manifestDocument.Assets...)
	manifest, err = json.MarshalIndent(manifestDocument, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	manifest = append(manifest, '\n')
	manifestDigest := sha256.Sum256(manifest)
	checksums := fmt.Sprintf("%x  %s\n%x  %s\n", digest, archiveName, manifestDigest, releaseManifestName)
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		switch filepath.Base(request.URL.Path) {
		case archiveName:
			_, _ = response.Write(archive)
		case "SHA256SUMS.txt":
			_, _ = response.Write([]byte(checksums))
		case releaseManifestName:
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
		default:
			http.NotFound(response, request)
		}
	}))
	defer server.Close()

	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	home := filepath.Join(directory, "home")
	dataHome := filepath.Join(home, ".local", "share")
	installDir := filepath.Join(home, ".local", "bin")
	if err := os.MkdirAll(home, 0o755); err != nil {
		t.Fatal(err)
	}
	environment := append(os.Environ(),
		"HOME="+home,
		"XDG_DATA_HOME="+dataHome,
		"PULS_INSTALL_DIR="+installDir,
		"PULS_INSTALL_REPOSITORY_URL="+server.URL,
	)
	installer := filepath.Join(root, "scripts", "install.sh")
	if runtime.GOOS == "darwin" {
		linkedApplications := filepath.Join(directory, "linked-applications")
		if err := os.MkdirAll(linkedApplications, 0o755); err != nil {
			t.Fatal(err)
		}
		applicationsLink := filepath.Join(home, "Applications")
		if err := os.Symlink(linkedApplications, applicationsLink); err != nil {
			t.Fatal(err)
		}
		rejected := exec.Command("sh", installer, "--version", version, "--no-path-update")
		rejected.Env = environment
		output, err := rejected.CombinedOutput()
		if err == nil || !strings.Contains(string(output), "не является безопасным каталогом") {
			t.Fatalf("installer followed a symlinked Applications directory: %v\n%s", err, output)
		}
		if _, err := os.Stat(filepath.Join(linkedApplications, "Puls.app")); !os.IsNotExist(err) {
			t.Fatalf("installer created bundle through symlinked Applications: %v", err)
		}
		if _, err := os.Stat(filepath.Join(installDir, "puls")); !os.IsNotExist(err) {
			t.Fatalf("installer changed binary before Applications validation: %v", err)
		}
		if err := os.Remove(applicationsLink); err != nil {
			t.Fatal(err)
		}

		appBundle := filepath.Join(home, "Applications", "Puls.app")
		plistPath := filepath.Join(appBundle, "Contents", "Info.plist")
		if err := os.MkdirAll(filepath.Dir(plistPath), 0o755); err != nil {
			t.Fatal(err)
		}
		const unmanaged = `<plist><dict><key>CFBundleIdentifier</key><string>io.github.cheviiot.puls</string></dict></plist>`
		if err := os.WriteFile(plistPath, []byte(unmanaged), 0o644); err != nil {
			t.Fatal(err)
		}
		rejected = exec.Command("sh", installer, "--version", version, "--no-path-update")
		rejected.Env = environment
		output, err = rejected.CombinedOutput()
		if err == nil || !strings.Contains(string(output), "не принадлежит установщику Puls") {
			t.Fatalf("installer claimed an unmanaged Puls.app: %v\n%s", err, output)
		}
		if content, err := os.ReadFile(plistPath); err != nil || string(content) != unmanaged {
			t.Fatalf("unmanaged Puls.app was modified: %q, %v", content, err)
		}
		if _, err := os.Stat(filepath.Join(installDir, "puls")); !os.IsNotExist(err) {
			t.Fatalf("installer changed binary before shortcut ownership validation: %v", err)
		}
		if err := os.RemoveAll(appBundle); err != nil {
			t.Fatal(err)
		}
	}
	command := exec.Command("sh", installer, "--version", version, "--no-path-update")
	command.Env = environment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("GUI install failed: %v\n%s", err, output)
	}
	managedPaths := []string{filepath.Join(installDir, "puls")}
	if runtime.GOOS == "linux" {
		desktopPath := filepath.Join(dataHome, "applications", "io.github.cheviiot.puls.desktop")
		desktop, err := os.ReadFile(desktopPath)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(desktop), "Exec=\""+installDir+"/puls\" gui") || !strings.Contains(string(desktop), "X-Puls-Managed=true") {
			t.Fatalf("desktop entry = %q", desktop)
		}
		iconPath := filepath.Join(dataHome, "icons", "hicolor", "512x512", "apps", "io.github.cheviiot.puls.png")
		if content, err := os.ReadFile(iconPath); err != nil || string(content) != "icon" {
			t.Fatalf("installed icon = %q, %v", content, err)
		}
		managedPaths = append(managedPaths, desktopPath, iconPath)
	} else {
		appBundle := filepath.Join(home, "Applications", "Puls.app")
		launcher, err := os.ReadFile(filepath.Join(appBundle, "Contents", "MacOS", "Puls"))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(launcher), "\""+installDir+"/puls\" gui") {
			t.Fatalf("macOS launcher = %q", launcher)
		}
		plist, err := os.ReadFile(filepath.Join(appBundle, "Contents", "Info.plist"))
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(plist), "<key>PulsInstallerManaged</key><true/>") {
			t.Fatalf("macOS app is missing installer marker: %q", plist)
		}
		managedPaths = append(managedPaths, appBundle)
	}

	command = exec.Command("sh", installer, "--uninstall", "--no-path-update")
	command.Env = environment
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("GUI uninstall failed: %v\n%s", err, output)
	}
	for _, path := range managedPaths {
		if _, err := os.Stat(path); !os.IsNotExist(err) {
			t.Errorf("uninstall left %s: %v", path, err)
		}
	}
}

func TestShellInstallerPreservesUnmanagedMacOSApp(t *testing.T) {
	if runtime.GOOS != "darwin" {
		t.Skip("macOS ownership check")
	}
	root, err := findProjectRoot()
	if err != nil {
		t.Fatal(err)
	}
	home := t.TempDir()
	appBundle := filepath.Join(home, "Applications", "Puls.app")
	plistPath := filepath.Join(appBundle, "Contents", "Info.plist")
	if err := os.MkdirAll(filepath.Dir(plistPath), 0o755); err != nil {
		t.Fatal(err)
	}
	const unmanaged = `<plist><dict><key>CFBundleIdentifier</key><string>io.github.cheviiot.puls</string></dict></plist>`
	if err := os.WriteFile(plistPath, []byte(unmanaged), 0o644); err != nil {
		t.Fatal(err)
	}
	command := exec.Command("sh", filepath.Join(root, "scripts", "install.sh"), "--uninstall", "--no-path-update")
	command.Env = append(os.Environ(), "HOME="+home, "PULS_INSTALL_DIR="+filepath.Join(home, ".local", "bin"))
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("uninstall failed: %v\n%s", err, output)
	}
	if content, err := os.ReadFile(plistPath); err != nil || string(content) != unmanaged {
		t.Fatalf("unmanaged Puls.app was modified: %q, %v", content, err)
	}

	if err := os.RemoveAll(appBundle); err != nil {
		t.Fatal(err)
	}
	externalContents := filepath.Join(home, "external-contents")
	if err := os.MkdirAll(filepath.Join(externalContents, "MacOS"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(externalContents, "Resources"), 0o755); err != nil {
		t.Fatal(err)
	}
	const forgedMarker = `<plist><dict><key>CFBundleIdentifier</key><string>io.github.cheviiot.puls</string><key>PulsInstallerManaged</key><true/></dict></plist>`
	externalPlist := filepath.Join(externalContents, "Info.plist")
	if err := os.WriteFile(externalPlist, []byte(forgedMarker), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(appBundle, 0o755); err != nil {
		t.Fatal(err)
	}
	contentsLink := filepath.Join(appBundle, "Contents")
	if err := os.Symlink(externalContents, contentsLink); err != nil {
		t.Fatal(err)
	}
	command = exec.Command("sh", filepath.Join(root, "scripts", "install.sh"), "--uninstall", "--no-path-update")
	command.Env = append(os.Environ(), "HOME="+home, "PULS_INSTALL_DIR="+filepath.Join(home, ".local", "bin"))
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("uninstall with symlinked Contents failed: %v\n%s", err, output)
	}
	if _, err := os.Lstat(contentsLink); err != nil {
		t.Fatalf("unmanaged bundle was deleted: %v", err)
	}
	if content, err := os.ReadFile(externalPlist); err != nil || string(content) != forgedMarker {
		t.Fatalf("external plist was modified: %q, %v", content, err)
	}
}

func TestShellInstallerRejectsWrongManifestPackage(t *testing.T) {
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
	wrongName := fmt.Sprintf("Puls_%s_%s_%s-wrong.tar.gz", version, runtime.GOOS, runtime.GOARCH)
	manifest := testReleaseManifest(t, version, runtime.GOOS, runtime.GOARCH, wrongName, [sha256.Size]byte{})
	server := httptest.NewServer(http.HandlerFunc(func(response http.ResponseWriter, request *http.Request) {
		if filepath.Base(request.URL.Path) == releaseManifestName {
			response.Header().Set("Content-Type", "application/json")
			_, _ = response.Write(manifest)
			return
		}
		http.NotFound(response, request)
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
		t.Fatalf("install.sh accepted a wrong manifest package:\n%s", output)
	}
	if !strings.Contains(string(output), "указывает неожиданный пакет") {
		t.Fatalf("unexpected install.sh error:\n%s", output)
	}
	if _, statErr := os.Stat(filepath.Join(installDir, "puls")); !os.IsNotExist(statErr) {
		t.Fatalf("installer left a binary after manifest failure: %v", statErr)
	}
}

func testReleaseManifest(t *testing.T, version, targetOS, targetArch, name string, digest [sha256.Size]byte) []byte {
	return testReleaseManifestWithCapabilities(t, version, targetOS, targetArch, name, digest, []string{"cli"})
}

func testReleaseManifestWithCapabilities(t *testing.T, version, targetOS, targetArch, name string, digest [sha256.Size]byte, capabilities []string) []byte {
	t.Helper()
	payload, err := json.MarshalIndent(releaseManifest{
		SchemaVersion: 2,
		Product:       "Puls",
		Version:       version,
		Assets: []releaseManifestAsset{{
			OS: targetOS, Arch: targetArch, File: name, SHA256: fmt.Sprintf("%x", digest), Kind: "archive", Capabilities: capabilities,
		}},
	}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	return append(payload, '\n')
}
