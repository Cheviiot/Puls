package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

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
