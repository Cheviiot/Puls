package main

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

type archiveFile struct {
	Name string
	Path string
	Mode os.FileMode
}

func releaseArchiveFiles(root, directory, binaryName, binaryPath string, includeGUIAssets bool) ([]archiveFile, error) {
	files := []archiveFile{{
		Name: filepath.ToSlash(filepath.Join(directory, binaryName)),
		Path: binaryPath,
		Mode: 0o755,
	}}
	if includeGUIAssets {
		for _, asset := range []string{"internal/gui/assets/Icon.png", "internal/gui/assets/Icon.svg", "internal/gui/assets/Icon.ico"} {
			path := filepath.Join(root, filepath.FromSlash(asset))
			info, err := os.Stat(path)
			if err != nil {
				return nil, fmt.Errorf("проверка GUI-ресурса %s: %w", asset, err)
			}
			if !info.Mode().IsRegular() {
				return nil, fmt.Errorf("GUI-ресурс %s не является обычным файлом", asset)
			}
			files = append(files, archiveFile{
				Name: filepath.ToSlash(filepath.Join(directory, "assets", filepath.Base(asset))),
				Path: path,
				Mode: 0o644,
			})
		}
	}
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
