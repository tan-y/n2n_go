package cli

import (
	"fmt"
	"runtime"

	"n2n-go/internal/n2n"
)

// Version is the n2n-go release version.
const Version = "0.1.0"

func runVersion(args []string) int {
	_ = args
	fmt.Printf("n2n-go %s\n", Version)
	fmt.Printf("n2n    %s\n", n2n.Version())
	fmt.Printf("go     %s\n", runtime.Version())
	return 0
}
