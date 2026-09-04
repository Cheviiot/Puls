package service

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"strings"
)

// HTTPStatusError preserves an unexpected response status for typed error
// classification without inspecting human-readable text.
type HTTPStatusError struct {
	StatusCode int
	Status     string
	Operation  string
}

func (e *HTTPStatusError) Error() string {
	if e == nil {
		return "неожиданное состояние HTTP"
	}
	return fmt.Sprintf("%s вернул состояние %s", e.Operation, e.Status)
}

// ReadLimited consumes at most limit bytes and rejects truncated oversized
// responses instead of silently accepting their prefix.
func ReadLimited(reader io.Reader, limit int64) ([]byte, error) {
	if limit < 1 {
		return nil, errors.New("предел ответа должен быть положительным")
	}
	limited := &io.LimitedReader{R: reader, N: limit + 1}
	payload, err := io.ReadAll(limited)
	if err != nil {
		return nil, err
	}
	if int64(len(payload)) > limit {
		return nil, fmt.Errorf("ответ превышает безопасный предел %d байт", limit)
	}
	return payload, nil
}

// DecodeJSONLimited validates the size and requires exactly one JSON value.
func DecodeJSONLimited(reader io.Reader, limit int64, destination any) error {
	payload, err := ReadLimited(reader, limit)
	if err != nil {
		return err
	}
	decoder := json.NewDecoder(bytes.NewReader(payload))
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	return EnsureJSONEOF(decoder)
}

// EnsureJSONEOF rejects a second JSON value while allowing trailing space.
func EnsureJSONEOF(decoder *json.Decoder) error {
	var extra any
	err := decoder.Decode(&extra)
	switch {
	case errors.Is(err, io.EOF):
		return nil
	case err == nil:
		return errors.New("ответ содержит несколько значений JSON")
	default:
		return err
	}
}

// ValidateContentType checks a response media type while ignoring optional
// parameters such as charset.
func ValidateContentType(header http.Header, expected ...string) error {
	raw := header.Get("Content-Type")
	mediaType, _, err := mime.ParseMediaType(raw)
	if err == nil {
		for _, allowed := range expected {
			if strings.EqualFold(mediaType, allowed) {
				return nil
			}
		}
	}
	return fmt.Errorf("неожиданный Content-Type %q", raw)
}

// DrainAndClose releases a response body for transport reuse without reading
// an unbounded error response.
func DrainAndClose(body io.ReadCloser) {
	if body == nil {
		return
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(body, 64<<10))
	_ = body.Close()
}

// ClassifyHTTPStatus maps an unexpected status to a stable error code.
func ClassifyHTTPStatus(status int) (ErrorCode, bool) {
	switch {
	case status == http.StatusUnauthorized || status == http.StatusForbidden:
		return CodeAuth, true
	case status == http.StatusRequestTimeout || status == http.StatusTooManyRequests || status >= 500:
		return CodeUnavailable, true
	default:
		return CodeProtocol, false
	}
}
