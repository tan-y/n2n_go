package main

import (
	"os"

	"n2n-go/internal/cli"
)

func main() {
	os.Exit(cli.Run(os.Args[1:]))
}
