package bench

// Phases in order. Each maps to a step of go/question.md; phase 0 is the
// instrument every later number is read through.
var Phases = []struct {
	N     int
	Title string
	Run   func()
}{
	{0, "the instrument", Phase0},
	{1, "the runtime already wrote this queue (G1)", Phase1},
	{2, "port the slot protocol (G2)", Phase2},
	{3, "build the sleep without a futex (G3)", Phase3},
	{4, "preemption you did not schedule (G4)", Phase4},
	{5, "close is a panic on the send side (G5)", Phase5},
	{6, "measure the tail you promised (G6)", Phase6},
}
