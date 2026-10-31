package turnstile_test

import (
	"errors"
	"fmt"
	"sync"

	"turnstile/internal/turnstile"
)

// Each goroutine takes its own handle. Close tells the consumers to stop once
// they have taken everything accepted before it.
func Example() {
	q := turnstile.New[string](8)

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		c := q.Consumer()
		for {
			v, err := c.Pop()
			if errors.Is(err, turnstile.ErrClosed) {
				return
			}
			fmt.Println(v)
		}
	}()

	p := q.Producer()
	for _, v := range []string{"reserve", "publish", "release"} {
		if err := p.Push(v); err != nil {
			fmt.Println(err)
		}
	}
	q.Close()
	wg.Wait()

	fmt.Println(p.Push("late"))
	// Output:
	// reserve
	// publish
	// release
	// turnstile: closed
}
