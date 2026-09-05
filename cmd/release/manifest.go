package main

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
)

type releaseManifest struct {
	SchemaVersion int                    `json:"schema_version"`
	Product       string                 `json:"product"`
	Version       string                 `json:"version"`
	Assets        []releaseManifestAsset `json:"assets"`
}

type releaseManifestAsset struct {
	OS           string   `json:"os"`
	Arch         string   `json:"arch"`
	File         string   `json:"file"`
	SHA256       string   `json:"sha256"`
	Kind         string   `json:"kind"`
	Capabilities []string `json:"capabilities"`
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
		if item.Kind == "" || len(item.Capabilities) == 0 {
			return artifact{}, fmt.Errorf("артефакт %q не содержит тип или возможности", item.Name)
		}
		assets = append(assets, releaseManifestAsset{
			OS: item.Target.OS, Arch: item.Target.Arch, File: item.Name, SHA256: fmt.Sprintf("%x", item.Digest),
			Kind: item.Kind, Capabilities: append([]string(nil), item.Capabilities...),
		})
	}
	payload, err := json.MarshalIndent(releaseManifest{
		SchemaVersion: 2,
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
