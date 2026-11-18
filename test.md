**Triangular Sum** ($1 + 2 + \dots + n$) or **Integer Multiplication** ($a \times b$) are much better starting targets for your specific VM than any of those three.

Look at why the original three fight against your `genesis.c` hardware and 8-byte `(in, out)` anchor setup:

* **Byte-wise parity & intra-byte XOR-fold** require bit-shifts or bit-masks (`AND`) to isolate bits inside a single byte—opcodes your VM doesn't even have (`PUSH`, `LOAD`, `ADD`, `XOR`, `LOOP`, `END`, `CALL`).
* **Multi-byte XOR-fold** needs multiple input bytes per test, which doesn't fit cleanly into your 1-byte-in, 1-byte-out header anchors (`body[12..19]`).
* **Affine cipher** ($a \cdot x + b$) works with repeated `ADD`, but doesn't force the engine to discover `LOOP`—it can just unroll a few `ADD`s in a straight line.

**Why Triangular Sum ($f(n) = n(n+1)/2 \bmod 256$) is the sweet spot:**

* **Fits 1-byte anchors natively:** Four tiny `(in, out)` pairs—like `(0, 0)`, `(3, 6)`, `(5, 15)`, and `(10, 55)`—completely prevent a program from cheating with hardcoded `ADD`s or lookup tricks.
* **Forces `LOOP` discovery:** Because the number of additions depends on the *value* of the input byte $n$, the only compact way to pass `(3, 6)` and `(10, 55)` is to actually evolve a working `LOOP ... END` countdown using `ADD` (plus subtracting 1 via `ADD 255`).
* **Smooth stepping stones:** Mutants that get small inputs right (`0`, `1`, `3`) can pass 3/4 anchors during your relaxed breeding check before locking in the general loop for larger inputs.

If you want a **stepping-stone warm-up** before jumping straight to dynamic loops, **Polynomial Evaluation** ($x^2$ or $3x + 5$ via repeated `ADD` + `XOR` mixing) lets you verify that your 4-anchor gate and `adapt_prior()` weights work cleanly before testing `LOOP` discovery.

Here are the 4 anchor pairs for your header (`body[12..19]`) and the exact minimal bytecode strand for Triangular Sum ($f(n) = 1 + 2 + \dots + n \bmod 256$), along with one critical check on how your VM handles stack Manipulation.

### 1. The 4 Byte-Anchor Pairs (`body[12..19]`)

Set the 8 anchor bytes in your header to `{0, 0, 1, 1, 4, 10, 10, 55}`:

* **`(0, 0)`** — Forces the loop to skip cleanly when $n = 0$ instead of underflowing to 255 iterations.
* **`(1, 1)`** — Rewards early 1-step mutants (like the identity function) so your relaxed breeding gate has a stepping stone.
* **`(4, 10)`** — Breaks simple linear rules like $f(n) = n$ or $f(n) = 2n$.
* **`(10, 55)`** — Breaks unrolled straight-line `ADD` chains, forcing a genuine `LOOP ... END` structure.

*(Bonus check: if you want to test 8-bit wrap-around too, swap `(4, 10)` for `(23, 20)` since $23 \times 24 / 2 = 276 \equiv 20 \pmod{256}$.)*

### 2. Minimal Surviving Strand in `genesis.c` Opcodes

Because your opcode set (`PUSH`, `LOAD`, `ADD`, `XOR`, `LOOP`, `END`, `CALL`) doesn't include stack shuffling (`DUP`/`SWAP`) or local `STORE`, how the strand looks depends on how `LOAD` and `LOOP` work in your interpreter:

**Case A: `LOOP` peeks/decrements a counter or `LOAD` reads stack/slots (`LOAD 0` = input/counter)**
If `LOOP` checks the top of stack (or a loop register) and `LOAD` can fetch the current loop index $i$ (counting down $n \to 1$):

```text
PUSH 0       // Initialize running sum = 0
LOAD 0       // Load input n as loop counter
LOOP         // While counter != 0:
  LOAD 1     //   Fetch current counter value (n, n-1, ..., 1)
  ADD        //   sum = (sum + current_counter) & 0xFF
END          // Decrement counter; jump back to LOOP if non-zero

```

* **Total length:** **6 instructions** (8 bytes if `PUSH` and `LOAD` take 1-byte immediate operands, or 6 bytes if operands are packed into low nibbles).
* **Trace on `(4, 10)`:** Starts with `sum = 0`, `counter = 4` $\rightarrow$ adds `4` (`4`) $\rightarrow$ adds `3` (`7`) $\rightarrow$ adds `2` (`9`) $\rightarrow$ adds `1` (`10`) $\rightarrow$ exits loop with `10` on top of the stack.

**Case B: Pure stack VM where `LOAD` *only* reads the static initial input $n$**
Watch out for a subtle trap here! If `LOAD` only ever pushes the original immutable input $n$—and `LOOP` consumes its counter internally without exposing the changing index to the stack—then **every iteration can only see $n$, not $n-1, n-2, \dots, 1$**.

* In that pure-immutable-`LOAD` setup, the VM can effortlessly compute **$n^2$** (`PUSH 0`, `LOAD`, `LOOP`, `LOAD`, `ADD`, `END` — adding $n$ to itself $n$ times!), which is an **even cleaner 6-op target** if `LOOP` hides its counter!
* To compute Triangular Sum when `LOAD` is read-only, the strand either needs `LOOP` to expose its counter on the stack *or* maintain `[sum, current_val]` on the stack, which requires `DUP`/`SWAP` or a `STORE` slot so `ADD 255` ($-1 \bmod 256$) can decrement the value each pass.

### Quick Sanity Check Before You Run

Check two lines in your `genesis.c` VM `switch` statement:

1. Does `LOAD` read from a stack offset (`sp - arg`), a mutable memory cell, or just the fixed input byte?
2. Does `LOOP` pop the counter into a hidden C variable `loop_cnt`, or leave the counter accessible on the stack?

If `LOOP` hides the counter in C and `LOAD` only reads the fixed input byte, use **Square ($f(n) = n^2 \bmod 256$)** with anchors **`{0, 0, 2, 4, 5, 25, 17, 33}`** ($17^2 = 289 \equiv 33$)—it forces the exact same `LOOP ... END` discovery in just 6 instructions with zero extra opcodes needed.

Triangular sum (\(\sum_{i=1}^n i = \frac{n(n+1)}{2} \bmod 256\)) is strictly superior for evolving LOOP and adaptive priors. Bitwise parity, affine ciphers, and XOR-folds all collapse into flat, loop-free straight-line code or lacks input-dependent iteration depth.

Why Triangular Sum Hits the Evolutionary Sweet Spot


Target Function	Loop Requirement	Unrolling Vulnerability	Fuel Selection Gradient
Affine Cipher (ax + b)	None (\(O(1)\) straight-line).	N/A — never needs LOOP (0x07).	Selects against LOOP to save branch fuel.
Byte Parity / XOR-Fold	Fixed 8-bit width.	Easily unrolled into 8 flat XORs.	Flat code beats LOOP because backward branches pay a depth penalty.
Triangular Sum (T(n))	Variable n iterations.	Impossible to unroll across diverse 1-byte anchors (e.g., n = 0, 5, 13, 37).	Forces true LOOP discovery, since our ISA has no MUL or SHR to compute n(n+1)/2 closed-form.


Three Structural Advantages in genesis.c

• Anti-Unrolling Anchor Spread: Four compact (in, out) byte-anchors like (0, 0), (5, 15), (13, 91), and (37, 191) (703 & 0xFF) fit inside just 8 header bytes (body[12..19]). Because no straight-line ADD chain of fixed length can satisfy both n = 5 and n = 37, only a dynamic counter loop passes 4/4 load-time verification. [1] (https://github.com/Moissanite-Labs/MDE/#:~:text=No%20shipped%20inference%20runtime%20projects,what%20the%20host%20will%20do.)
• Rich Stepping-Stone Landscape (>= 3/4 Lexicase): With (0, 0) and (1, 1) in the anchor set, trivial identity (LOAD 0, EMIT) passes early anchors immediately. That gives adapt_prior() live gradient signal to boost ADD (0x03) and LOOP/END (0x07/0x08) instead of starving in an all-or-nothing fitness desert.
• Stack-Swap Pressure: Computing acc += n; n -= 1 while keeping Δsp == 0 across LOOP ... END exposes whether our 9-codon ISA has enough stack ergonomics (such as needing DUP / SWAP vs. scratch-pad re-loads) without hiding behind algebraic shortcuts.

One ISA Catch to Watch

Look closely at our current 9 opcodes (PUSH, LOAD, ADD, XOR, EMIT, ASSERT, LOOP, END, CALL). Without DUP or SWAP, decrementing a loop counter n (PUSH 0xFF, ADD) consumes n unless it can be duplicated for both accumulating acc += n and testing END (n != 0).
Adding a single 1-byte DUP2 (0x0A: [a, b] -> [a, b, a, b] or DUP/OVER) gives LOOP everything required to express clean, minimal triangular summation (acc, n state pair) in ~9 bytes of bytecode while preserving strict Δsp == 0 load-time verification. [1] (https://github.com/Moissanite-Labs/MDE/#:~:text=Open:%20how%20to%20bind%20a,the%20reported%20benchmark%20gains%20reproduce.)
Should I wire the 8-byte Triangular Sum anchor gate into genesis.c alongside DUP/OVER stack codons so you can watch the fuel tournament compress a bloated accumulator loop in real time?
