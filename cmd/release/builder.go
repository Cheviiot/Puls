package main

import (
	"crypto/sha256"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

type artifact struct {
	Name   string
	Digest [sha256.Size]byte
	Target target
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
