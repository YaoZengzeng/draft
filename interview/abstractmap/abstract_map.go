package main

import (
	"fmt"
	"sync"
	"time"
)

type callback func(key string, value string)
type item struct{
	value 	string
	seq 	int
}

const (
	bufferSize = 1024
	timeoutMinutes = 60
)

// To simpilify, the type of key and value is string.
// We could use interface{} to make this library more general.
type AbstractMap struct {
	sync.RWMutex

	m map[string]item

	seqNow 		int
	seqStamp 	[]int

	lruCallback callback
	timeoutCallback callback
}

func NewAbstractMap(lruCallback callback, timeoutCallback callback) *AbstractMap {
	m := &AbstractMap{
		m: 					make(map[string]item),
		seqNow: 			0,
		seqStamp:			[]int{},
		lruCallback: 		lruCallback,
		timeoutCallback: 	timeoutCallback,
	}

	go m.watchTimeout()

	return m
}

func (m *AbstractMap) watchTimeout() {
	for {
		select {
		// GC per minute.
		case <-time.After(1 * time.Minute):
			m.seqStamp = append(m.seqStamp, m.seqNow)
			if len(m.seqStamp) < timeoutMinutes {
				break
			}
			m.Lock()
			for k, v := range m.m {
				// Delete the timeout item.
				if v.seq <= m.seqStamp[0] {
					go m.timeoutCallback(k, v.value)
					delete(m.m, k)
				}
			}
			m.seqStamp = m.seqStamp[1:]
			m.Unlock()
		}
	}
}

func (m *AbstractMap) Get(k string) (string, bool) {
	m.RLock()
	defer m.RUnlock()

	v, ok := m.m[k]

	return v.value, ok
}

func (m *AbstractMap) Put(k string, v string) {
	m.Lock()
	defer m.Unlock()
	m.seqNow += 1
	if len(m.m) == bufferSize {
		var minKey string
		var minSeq int
		firstloop := true
		for key, value := range m.m {
			if firstloop {
				minKey = key
				minSeq = value.seq
				firstloop = false
			}
			if value.seq < minSeq {
				minKey = key
				minSeq = value.seq
			}
		}
		go m.lruCallback(minKey, m.m[minKey].value)
		delete(m.m, minKey)

	}
	m.m[k] = item{
		value:	v,
		seq:	m.seqNow,
	}
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

	m.Put("key1", "value1")
	v, _ := m.Get("key1")
	fmt.Printf("value of key1 is %s\n", v)

}
