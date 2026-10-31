package bench

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// Some arms need a runtime setting a process cannot change in itself:
// GODEBUG=asyncpreemptoff is read when the runtime starts. Those arms run as
// child processes of this binary, launched back to back with the arm they are
// compared against, so the pair still shares a machine state.

var children = map[string]func() any{}

// ChildMain runs one registered experiment and prints its result as JSON.
func ChildMain(name string) {
	f, ok := children[name]
	if !ok {
		fmt.Fprintf(os.Stderr, "unknown child experiment %q\n", name)
		os.Exit(2)
	}
	b, err := json.Marshal(f())
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	fmt.Printf("CHILD %s\n", b)
}

// inChild runs experiment `name` in a fresh process with `env` added, and
// decodes its result into out. The child inherits the calibration.
func inChild(name string, env []string, out any) error {
	exe, err := os.Executable()
	if err != nil {
		return err
	}
	cmd := exec.Command(exe, "-child", name)
	cmd.Env = append(append(os.Environ(), env...), fmt.Sprintf("TURNSTILE_NS_PER_TICK=%v", Clk.NsPerTick()))
	cmd.Stderr = os.Stderr
	b, err := cmd.Output()
	if err != nil {
		return fmt.Errorf("child %s: %w", name, err)
	}
	for _, line := range strings.Split(string(b), "\n") {
		if rest, ok := strings.CutPrefix(strings.TrimSpace(line), "CHILD "); ok {
			return json.Unmarshal([]byte(rest), out)
		}
	}
	return errors.New("child " + name + " printed no result")
}

// asyncEnv is the GODEBUG for each side of the asynchronous-preemption A/B.
func asyncEnv(on bool) []string {
	if on {
		return []string{"GODEBUG=asyncpreemptoff=0"}
	}
	return []string{"GODEBUG=asyncpreemptoff=1"}
}
