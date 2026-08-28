//go:build !race

package phases

// raceEnabled is false in a normal build. See race_on.go.
const raceEnabled = false
