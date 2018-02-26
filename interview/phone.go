package main

import (
	"fmt"
	"os"
	"bufio"
	"log"
)

var keyboard map[string]string

func main() {
	keyboard = map[string]string{
		"0": "0",
		"1": "1",
		"2": "abc",
		"3": "def",
		"4": "ghi",
		"5": "jkl",
		"6": "mno",
		"7": "pqrs",
		"8": "tuv",
		"9": "wxyz",
	}

	input := bufio.NewReader(os.Stdin)
	fmt.Printf("Please enter phone number:\n")
	numbers, err := input.ReadString('\n')
	if err != nil {
		log.Fatalf("Read input failed: %v", err)
		return
	}
	if len(numbers) > 1 {
		// delete the '\n'.
		numbers = numbers[:len(numbers) - 1]
	} else {
		return
	}

	result := ""
	dfs(numbers, result)

	return
}

func dfs(numbers string, result string) {
	s, ok := keyboard[string(numbers[0])]
	if !ok {
		log.Fatalf("Invalid phone numbers")
	}

	for _, a := range s {
		if len(numbers) > 1 {
			dfs(numbers[1:], result + string(a))
		} else {
			fmt.Println(result + string(a))
		}
	}
}

