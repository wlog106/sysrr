// Go: a nil pointer panic.  The runtime has its own threads, its own signal
// handler for the fault, and raw syscalls.
package main

import (
	"fmt"
	"os"
	"time"
)

type node struct{ next *node; v uint64 }

func main() {
	rnd, _ := os.Open("/dev/urandom")
	t0 := time.Now()
	var sum uint64
	b := make([]byte, 8)
	for i := 0; ; i++ {
		rnd.Read(b)
		for _, x := range b {
			sum = sum*31 + uint64(x)
		}
		fmt.Printf("iter %d %016x\n", i, sum)
		time.Sleep(20 * time.Millisecond)
		if time.Since(t0) > 3200*time.Millisecond {
			var n *node
			fmt.Println(n.next.v)
		}
	}
}
