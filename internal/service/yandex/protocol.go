package yandex

import (
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

var cacheBustCounter atomic.Uint64

func cacheBust(rawURL string) string {
	sep := "&"
	if !strings.Contains(rawURL, "?") {
		sep = "?"
	}
	value := strconv.FormatInt(time.Now().UnixNano(), 36) + "-" + strconv.FormatUint(cacheBustCounter.Add(1), 36)
	return rawURL + sep + "cb=" + value
}
