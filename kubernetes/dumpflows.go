package main

import (
	"bytes"
	"fmt"
	"os/exec"
	"regexp"
	"strings"
)

const (
	ovsCommandTimeout = 5
	ovsVsctlCommand   = "ovs-vsctl"
	ovsOfctlCommand   = "ovs-ofctl"
)

// RunOVSOfctl runs a command via ovs-vsctl.
func RunOVSOfctl(args ...string) (string, string, error) {
	cmdPath, err := exec.LookPath(ovsOfctlCommand)
	if err != nil {
		return "", "", err
	}

	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	cmdArgs := []string{fmt.Sprintf("--timeout=%d", ovsCommandTimeout)}
	cmdArgs = append(cmdArgs, args...)
	cmd := exec.Command(cmdPath, cmdArgs...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr

	err = cmd.Run()
	return stdout.String(), stderr.String(), err
}

func main() {
	stdout, _, err := RunOVSOfctl("dump-flows", "br-int")
	if err != nil {
		fmt.Printf("dump flows failed: %s\n", stdout)
		return
	}

	flows := strings.Split(stdout, "\n")

	re, err := regexp.Compile(`tp_dst=(.*?)[, ]`)
	if err != nil {
		fmt.Printf("regexp Compile failed\n")
		return
	}

	nodePortsInFlow := make(map[string]bool)
	for _, flow := range flows {
		group := re.FindStringSubmatch(flow)
		if group == nil {
			continue
		}

		if strings.Contains(flow, "tcp") {
			entry := fmt.Sprintf("tcp_%s", group[1])
			nodePortsInFlow[entry] = true
		} else if strings.Contains(flow, "udp") {
			entry := fmt.Sprintf("udp_%s", group[1])
			nodePortsInFlow[entry] = true
		}
	}

	fmt.Printf("node ports in flow is :\n%v\n", nodePortsInFlow)
}
