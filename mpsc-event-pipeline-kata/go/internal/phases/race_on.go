//go:build race

package phases

// raceEnabled is true under -race.
//
// The race detector deliberately de-intrinsifies sync/atomic: the compiler's
// intrinsic table is skipped for that package under -race, so every atomic
// becomes a real call into the ThreadSanitizer runtime. A -race build is
// therefore a different program by 5-20x, and any number it produces is a
// measurement of the instrumentation.
//
// The C++ answer has the equivalent problem with ASan and handles it by giving
// the sanitized target a distinct binary name, so the two cannot be confused in
// bin/. Here the binary refuses outright.
const raceEnabled = true
