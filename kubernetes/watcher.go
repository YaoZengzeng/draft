package main

import (
        "fmt"
        "time"

        utilwait "k8s.io/apimachinery/pkg/util/wait"

        informerfactory "k8s.io/client-go/informers"
        "k8s.io/client-go/kubernetes"
        "k8s.io/client-go/tools/cache"
        "k8s.io/client-go/tools/clientcmd"

        kapi "k8s.io/api/core/v1"

)

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
	
	resyncInterval := 3 * time.Second
	IFactory := informerfactory.NewSharedInformerFactory(clientset, resyncInterval)

	serviceInformer := IFactory.Core().V1().Services()

	StartServiceWatch := func(handler cache.ResourceEventHandler) {
		serviceInformer.Informer().AddEventHandler(handler)
		go serviceInformer.Informer().Run(utilwait.NeverStop)
	}

	StartServiceWatch(cache.ResourceEventHandlerFuncs{
		AddFunc: func(obj interface{}) {
			service, ok := obj.(*kapi.Service)
			if !ok {
				fmt.Printf("AddFunc: could not get service from obj\n")
				return
			}
			fmt.Printf("AddFunc: service is %v\n", service)

		},
		UpdateFunc: func(obj, new interface{}) {
			_, ok := obj.(*kapi.Service)
			if !ok {
				fmt.Printf("UpdateFunc: could not get service from obj\n")
				return
			}
			fmt.Printf("UpdateFunc: service is ...\n")
		},
		DeleteFunc: func(obj interface{}) {
			service, ok := obj.(*kapi.Service)
			if !ok {
				fmt.Printf("DeleteFunc: could not get service from obj\n")
				return
			}
			fmt.Printf("DeleteFunc: service is %v\n", service)
		},
	})

	fmt.Printf("start watching ...")

	select {}
}

