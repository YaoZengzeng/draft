package main

import (
	"fmt"
	"strings"

	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"

	kapi "k8s.io/api/core/v1"

	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

const TCP = "TCP"

const UDP = "UDP"

const apiserver = "127.0.0.1:8080"

func main() {
	config, err := clientcmd.BuildConfigFromFlags(apiserver, "")
	if err != nil {
		fmt.Printf("build config failed\n")
		return
	}

	clientset, err := kubernetes.NewForConfig(config)
	if err != nil {
		fmt.Printf("NewForConfig failed\n")
		return
	}

	existingNamespaces, err := clientset.Core().Namespaces().List(metav1.ListOptions{})
	if err != nil {
		fmt.Printf("clientset get namespaces failed\n")
		return
	}

	nodePorts := make(map[string]bool)

	for _, namespace := range existingNamespaces.Items {
		fmt.Printf("%s\n", namespace.ObjectMeta.Name)

		existingServices, err := clientset.Core().Services(namespace.ObjectMeta.Name).List(metav1.ListOptions{})
		if err != nil {
			fmt.Printf("clientset get list failed\n")
			return
		}

		for _, service := range existingServices.Items {
			if service.Spec.Type != kapi.ServiceTypeNodePort || len(service.Spec.Ports) == 0 {
				continue
			}

			for _, svcPort := range service.Spec.Ports {
				port := svcPort.NodePort
				if port == 0 {
					continue
				}

				prot := svcPort.Protocol
				if prot != TCP && prot != UDP {
					continue
				}

				protocol := strings.ToLower(string(prot))

				nodePortKey := fmt.Sprintf("%s_%d", protocol, port)

				nodePorts[nodePortKey] = true
			}
		}

	}

	fmt.Printf("node ports are:\n%v\n", nodePorts)
}
