// Package cli implements the n2n command line interface.
package cli

import (
	"fmt"
	"os"
)

const usageText = `n2n-go - macOS n2n edge client (first iteration)

Usage:
  n2n edge [options]    Start an edge connected to a supernode (requires root)
  n2n version           Print version information
  n2n help              Show this help

Run "n2n edge -h" for the list of edge options.
`

// Run dispatches the subcommand and returns the process exit code.
func Run(args []string) int {
	if len(args) == 0 {
		fmt.Fprint(os.Stderr, usageText)
		return 2
	}

	switch args[0] {
	case "edge":
		return runEdge(args[1:])
	case "version":
		return runVersion(args[1:])
	case "help", "-h", "--help":
		fmt.Fprint(os.Stdout, usageText)
		return 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n%s", args[0], usageText)
		return 2
	}
}
