package yandex

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"net/url"
	"strconv"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

var errInvalidWebsocketAcknowledgement = fmt.Errorf("%w: неверное подтверждение WebSocket", service.ErrProtocol)

func (p *Backend) websocketUpload(ctx context.Context, rawURL string, duration time.Duration, ready func(), record func(int64)) error {
	return p.websocketUploadWithTimeout(ctx, rawURL, 0, duration, ready, record)
}

func (p *Backend) websocketUploadWithTimeout(ctx context.Context, rawURL string, connectionTimeout, duration time.Duration, ready func(), record func(int64)) error {
	u, err := url.Parse(rawURL)
	if err != nil {
		return err
	}
	if !validWebSocketProbeURL(rawURL) {
		return service.ProtocolError(errors.New("неверный URL WebSocket upload-пробы"))
	}
	query := u.Query()
	query.Set("type", "upload")
	query.Set("duration", strconv.Itoa(max(1, int(math.Ceil(duration.Seconds()+2)))))
	u.RawQuery = query.Encode()
	header := http.Header{"User-Agent": []string{userAgent}, "Origin": []string{"https://yandex.ru"}}
	dialCtx := ctx
	cancelDial := func() {}
	if connectionTimeout > 0 {
		dialCtx, cancelDial = context.WithTimeout(ctx, connectionTimeout)
	}
	conn, response, err := p.dialer.DialContext(dialCtx, u.String(), header)
	cancelDial()
	if err != nil {
		if response != nil {
			_ = response.Body.Close()
		}
		return fmt.Errorf("подключение WebSocket: %w", err)
	}
	defer conn.Close()
	conn.SetReadLimit(maxWebsocketAckSize)
	closeDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-closeDone:
		}
	}()
	defer close(closeDone)
	writeErr := make(chan error, 1)
	writeDone := make(chan struct{})
	var sent atomic.Int64
	go func() {
		defer close(writeDone)
		for ctx.Err() == nil {
			if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
				writeErr <- err
				_ = conn.Close()
				return
			}

			sent.Add(websocketChunkSize)
			if err := conn.WriteMessage(websocket.BinaryMessage, websocketUploadChunk); err != nil {
				sent.Add(-websocketChunkSize)
				writeErr <- err
				_ = conn.Close()
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
	defer func() {
		_ = conn.Close()
		<-writeDone
	}()

	acked := int64(0)
	for {
		ackBytes, readErr := readWebsocketAcknowledgement(conn)
		if readErr != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			select {
			case err := <-writeErr:
				return fmt.Errorf("отправка через WebSocket: %w", err)
			default:
			}
			return fmt.Errorf("получение через WebSocket: %w", readErr)
		}
		available := sent.Load() - acked
		if available < 0 || ackBytes > available {
			return fmt.Errorf("%w: объём превышает отправленные данные", errInvalidWebsocketAcknowledgement)
		}

		if ackBytes == 0 {
			continue
		}
		ready()
		acked += ackBytes
		record(ackBytes)
	}
}

func readWebsocketAcknowledgement(conn *websocket.Conn) (int64, error) {
	messageType, reader, err := conn.NextReader()
	if err != nil {
		return 0, err
	}
	if messageType != websocket.TextMessage {
		return 0, fmt.Errorf("%w: неожиданный тип сообщения %d", errInvalidWebsocketAcknowledgement, messageType)
	}
	type acknowledgement struct {
		Kind  string `json:"k"`
		Bytes *int64 `json:"b"`
	}
	limited := &io.LimitedReader{R: reader, N: maxWebsocketAckSize + 1}
	decoder := json.NewDecoder(limited)
	var ack acknowledgement
	if err := decoder.Decode(&ack); err != nil {
		return 0, fmt.Errorf("%w: ошибка JSON: %v", errInvalidWebsocketAcknowledgement, err)
	}
	if err := service.EnsureJSONEOF(decoder); err != nil {
		return 0, fmt.Errorf("%w: ошибка JSON: %v", errInvalidWebsocketAcknowledgement, err)
	}
	if limited.N <= 0 {
		return 0, fmt.Errorf("%w: сообщение превышает безопасный предел", errInvalidWebsocketAcknowledgement)
	}
	if ack.Kind != "u" || ack.Bytes == nil || *ack.Bytes < 0 {
		return 0, fmt.Errorf("%w: неверная схема", errInvalidWebsocketAcknowledgement)
	}
	return *ack.Bytes, nil
}

func (p *Backend) httpUpload(ctx context.Context, postURL string, payload []byte, ready func(), record func(int64)) error {
	for ctx.Err() == nil {
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, cacheBust(postURL), bytes.NewReader(payload))
		if err != nil {
			return err
		}
		req.Header.Set("User-Agent", userAgent)
		req.Header.Set("Content-Type", "application/octet-stream")
		resp, err := p.client.Do(req)
		if err != nil {
			return err
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			_ = resp.Body.Close()
			return &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "HTTP-запрос отдачи"}
		}
		if resp.ContentLength > maxUploadResponse {
			_ = resp.Body.Close()
			return service.ProtocolError(errors.New("неожиданный размер ответа HTTP при отдаче: превышен безопасный предел"))
		}
		responseBytes, copyErr := io.Copy(io.Discard, io.LimitReader(resp.Body, maxUploadResponse+1))
		closeErr := resp.Body.Close()
		if copyErr != nil {
			return copyErr
		}
		if responseBytes > 1<<20 {
			return service.ProtocolError(errors.New("ответ HTTP при отдаче превышает безопасный предел"))
		}
		if closeErr != nil {
			return closeErr
		}
		ready()
		record(int64(len(payload)))
	}
	return ctx.Err()
}
