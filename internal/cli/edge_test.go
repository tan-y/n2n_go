package cli

import (
	"flag"
	"testing"
)

func TestMulticastFlags(t *testing.T) {
	tests := []struct {
		name    string
		initial bool // value loaded from the config file before flags are parsed
		args    []string
		want    bool // expected cfg.DropMulticast
		wantErr bool
	}{
		{name: "default keeps dropping multicast", initial: true, want: true},
		{name: "config file can accept multicast", initial: false, want: false},
		{name: "-E accepts multicast", initial: true, args: []string{"-E"}, want: false},
		{name: "-E=false keeps dropping", initial: true, args: []string{"-E=false"}, want: true},
		{name: "-E=true accepts multicast", initial: true, args: []string{"-E=true"}, want: false},
		{name: "--drop-multicast overrides an accepting file", initial: false, args: []string{"--drop-multicast"}, want: true},
		{name: "--drop-multicast=false accepts multicast", initial: true, args: []string{"--drop-multicast=false"}, want: false},
		{name: "last flag wins: -E then --drop-multicast", initial: true, args: []string{"-E", "--drop-multicast"}, want: true},
		{name: "last flag wins: --drop-multicast then -E", initial: true, args: []string{"--drop-multicast", "-E"}, want: false},
		{name: "last flag wins: -E then -E=false", initial: true, args: []string{"-E", "-E=false"}, want: true},
		{name: "invalid -E value", initial: true, args: []string{"-E=bogus"}, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			drop := tc.initial
			fs := flag.NewFlagSet("edge", flag.ContinueOnError)
			fs.SetOutput(nopWriter{})
			registerMulticastFlags(fs, &drop)

			err := fs.Parse(tc.args)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("-E=bogus: expected a parse error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("parse %v: %v", tc.args, err)
			}
			if drop != tc.want {
				t.Errorf("parse %v with initial %v: DropMulticast = %v, want %v", tc.args, tc.initial, drop, tc.want)
			}
		})
	}
}

type nopWriter struct{}

func (nopWriter) Write(p []byte) (int, error) { return len(p), nil }
