package abstract

import (
	"sync"
	"time"
)

type callback func(key string, value string)

// To simpilify, the type of key and value is string.
// We could use interface{} to make this library more general.
type AbstractMap struct {
	sync.RWMutex

	m map[string]string
	timeout map[time.Time]string

	lruCallback callback
	timeoutCallback callback
}

func NewAbstractMap(lruCallback callback, timeoutCallback callback) *AbstractMap {
	return &AbstractMap{
		m: make(map[string]string, 16),
		timeout: make(map[time.Time]string, 16),
		lruCallback: lruCallback,
		timeoutCallback: timeoutCallback,
	}
}

func (m *AbstractMap) Get(k string) (string, bool) {
	m.RLock()
	defer m.RUnlock()

	v, ok := m.m[k]

	return v, ok
}

func (m *AbstractMap) Put(k string, v string) {
	m.Lock()
	defer m.Unlock()
	m.m[k] = v
	m.timeout[time.Now] = k
}

func (m *AbstractMap) Remove(k string) {
	m.Lock()
	defer m.Unlock()
	delete(m.m, k)
}

func main() {
	lru := func(key string, value string) {
		fmt.Printf("lru callback delete key: %s\n", key)
	}
	timeout := func(key string, value string) {
		fmt.Printf("timeout callback delete key: %s\n", key)
	}
	m := NewAbstractMap(lru,timeout)
}
