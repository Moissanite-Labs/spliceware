The two complete design proposals from the second half of your tab—translated from the architectural trade-offs and anchor analysis into concrete, actionable specifications—cover **Two-Tiered Lexicase Anchor Gating** and **Triangular Sum Evolutionary Selection**.

---

## Proposal 1: Two-Tiered Lexicase Anchor Gating (Preventing Collapse Without Suffocating Discovery)

In a pure fuel-saving tournament, selection pressure inevitably favors the **trivial `HALT` collapse** (`0x00` at `CODE_START` burns zero fuel and wins every round). Conversely, a rigid all-or-nothing gate across a large test suite creates brittle search cliffs where valid two-step mutations die because step one temporarily breaks an assertion.

### 1. Memory & Wire Geometry

* **Header Carving:** Reserve **8 bytes** in the `Cell` header (`body[12..19]` in the 10-codon layout, or `body[14..21]` with `DUP`/`OVER`) for **4 compact `(in_byte, expected_out_byte)` anchor pairs**.
* **Zero-Control-Packet Trust:** Because the anchors travel inside the 1,088-byte `Cell` packet (`RAM == WIRE == HASH`), every UDP migrant cell is **self-verifying at load time** without requiring recursive Arena lookups or external test suites.
* **Computational Cost:** Increases per-mitosis evaluation from $2 \times \text{step\_branch()}$ to $2 \times (K + 1) \times \text{step\_branch()}$ ($K = 4$).

### 2. Two-Tiered Selection & Breeding Rule

| Tier | Gate Threshold | Action & Evolutionary Role |
| --- | --- | --- |
| **Tier 1: Canonical Promotion** | **Strict `4/4` anchors** + higher fuel margin | Promotes candidate to `champ` and broadcasts the new 1,088-byte `Cell` over UDP. Guarantees the primary lineage never regresses or collapses into `0x00`. |
| **Tier 2: Stepping-Stone Pool** | **Relaxed `>= 3/4` anchors** | Admits near-miss mutants into `ancestor_b` (the secondary breeding partner). Preserves intermediate `LOOP` structures and updated adaptive weights so `sacred_splice()` can bridge zero-fitness valleys on the next tick. |

---

## Proposal 2: Triangular Sum ($T(n) \bmod 256$) as the Evolutionary Target

Bitwise parity, XOR-folds, and affine ciphers ($ax + b$) fail to evolve genuine control flow: affine transforms require $O(1)$ straight-line math (selecting *against* `LOOP` to save branch fuel), while fixed-width byte parity unrolls into 8 flat `XOR` instructions.

### 1. Why $T(n) = \sum_{i=1}^{n} i = \frac{n(n+1)}{2} \bmod 256$ Forces True `LOOP` Discovery

* **Variable Iteration Depth:** Evaluating $n$ requires $n$ dynamic loop iterations.
* **Anti-Unrolling Anchor Spread:** Packing the four pairs **`(0, 0)`**, **`(5, 15)`**, **`(13, 91)`**, and **`(37, 191)`** (where $T(37) = 703 \equiv 191 \pmod{256}$) into the 8-byte anchor header makes straight-line unrolling impossible—no fixed-length `ADD` chain can satisfy both $n = 5$ and $n = 37$.
* **Smooth Stepping-Stone Gradient:** Including `(0, 0)` (and/or `(1, 1)`) allows trivial identity strands (`LOAD 0, EMIT`) to pass early anchors immediately, feeding live gradient signal into `adapt_prior()` to boost `ADD` (`0x03`) and `LOOP`/`END` (`0x07`/`0x08`) instead of starving in an all-or-nothing fitness desert.

### 2. Minimal ISA Stack Ergonomics (`DUP` / `OVER`)

* **The Bottleneck:** Without stack duplication, decrementing a loop counter $n$ (`PUSH 0xFF, ADD`) consumes $n$, preventing a loop from both accumulating `acc += n` and testing `END` (`n != 0`) while obeying the $\Delta sp == 0$ load-time invariant.
* **The Fix:** Adding 1-byte **`DUP` (`0x0A`: `[a] -> [a, a]`)** and **`OVER` (`0x0B`: `[a, b] -> [a, b, a]`)** lets the VM maintain an `(acc, n)` state pair on the stack and express a complete triangular summation loop in **~9 bytes of bytecode**, allowing the fuel tournament to compress bloated accumulator loops in real time.
