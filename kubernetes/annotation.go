package main

import (
	"encoding/json"
	"fmt"
	kapi "k8s.io/api/core/v1"
	"net/http"
)

const PODNAME = "abc"
const APISERVERIP = "127.0.0.1"

func getAnnotationOnPod(server, namespace, pod string) (map[string]string, error) {
	url := fmt.Sprintf("%s/api/v1/namespaces/%s/pods/%s", server, namespace, pod)

	res, err := http.Get(url)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	var apiPod kapi.Pod
	err = json.NewDecoder(res.Body).Decode(&apiPod)
	if err != nil {
		return nil, err
	}

	return apiPod.ObjectMeta.Annotations, nil
}

func main() {
	server := "http://" + APISERVERIP + ":8080"
	argAnnotations, err := getAnnotationOnPod(server, "default", PODNAME)
	if err != nil {
		fmt.Printf("get annotation failed: %v\n", err)
		return
	}

	fmt.Printf("annotation is %v\n", argAnnotations)

	return
}
