Mitosis and A/B testing collapse into a single topological primitive: symmetric dual-branching (FORK) paired with fuel-efficiency natural selection. You do not build a separate test harness, router, or telemetry service. A single active Cell splits into two parallel sibling reductions inside the flat Arena, and the branch that finishes valid work with the highest remaining fuel automatically wins the root pointer.
Note: Fixing a subtle bug from the previous snippet where C array field lengths ([32], [1024]) were elided—the delta below restores explicit array dimensions so every Cell stays strictly 1,088 bytes.
                     [ Input Cell X ]
                            │
                  ┌─────────┴─────────┐  <── MITOSIS (Synth emits Fork Cell)
                  ▼                   ▼
         [ Variant A | X ]   [ Variant B | X ]
         (Starts: 500 Fuel)  (Starts: 500 Fuel)
                  │                   │
                  ▼                   ▼
          [ Out_A : 412 ]     [ Out_B : 190 ]  (or Trap!)
                  │                   │
                  └─────────┬─────────┘  <── NATURAL SELECTION (Fuel Tournament)
                            ▼
                 [ Root slides to Out_A ] ──▶ UDP Mesh Spill

1. Mitosis Is Just a Fork Cell (body[0..1] == 0xFFFF)

Instead of replacing a broken operator with one guess, the Synthesizer Cell (synth_hash) undergoes mitosis: it emits two distinct mutant operators (Variant_A and Variant_B) and binds them into a single Fork Cell [Left: Variant_A | Right: Variant_B | Body[0..1]: 0xFFFF].
• Zero Extra Structs. 0xFFFF in the 16-bit fuel header acts as a topological marker inside the existing 1,088-byte Cell. It tells flow() that left and right are competing sibling operators rather than a single Operator -> Operand pair.
• Spontaneous Self-Division. Even without a trap, any hot Cell whose remaining fuel drops below a reproductive threshold can trigger mitosis—cloning its own bytecode into two child Cells with slight opcode perturbations (e.g., swapping a linear scan for an indexed offset) to search for faster paths.

2. A/B Testing Is an In-Arena Fuel Tournament

Traditional A/B testing requires external metrics pipelines, clocks, and percentage traffic splitters. In a deterministic, fuel-metered Arena, remaining fuel is the universal fitness metric (measuring exact compute efficiency), and ASSERT (0x06) is the correctness gate.
• Shadow Execution. When flow() encounters a Fork Cell applied to live input X, it steps both [Variant_A | X] and [Variant_B | X] against the exact same input bytes in the Arena.
• Invariant Equivalence. Both outputs must pass embedded ASSERT opcodes without trapping. You can also enforce strict semantic equivalence by requiring Hash(Payload_A) == Hash(Payload_B) when testing pure optimizations, or let embedded domain assertions grade novel behaviors.
• Deterministic Winner Selection. Each successful reduction writes its leftover fuel into out.body[0..1]. No wall-clock timers (which break P2P determinism) are needed:
	1. If one variant traps and the other halts cleanly, the surviving Cell wins immediately.
	2. If both halt cleanly, the Cell with higher remaining fuel wins.
	3. If remaining fuel is identical, the lexicographically higher 32-byte Cell hash breaks the tie identically across every node on Earth.

3. Mesh-Wide Distributed Mitosis (Swarm A/B)

Because every Cell spills across UDP on a cache miss, A/B testing naturally scales across peers without coordination:
• Split-Horizon Evaluation. Node 1 can evaluate Variant_A locally while spilling Variant_B's 32-byte application hash onto UDP. A neighbor picks up Variant_B, reduces it, and spills the 1,088-byte result back.
• Fitness-Weighted Gossip Convergence. Update the peer convergence rule from pure hash comparison to (Remaining_Fuel, Hash) comparison. When two nodes gossip competing valid reductions for the same causal parent, the leaner, higher-fuel lineage naturally overwrites the slower lineage across the entire P2P mesh.

The ~45-Line C Delta Inside flow()

Here is the exact structural fix and tournament reducer that drops straight into the Arena engine:
c
typedef struct __attribute__((packed)) {
    uint8_t left[32];     // Operator  OR Mitosis Variant A
    uint8_t right[32];    // Operand   OR Mitosis Variant B
    uint8_t body[1024];   // [0..1]: Fuel (0xFFFF == Mitosis Fork) | [2..1023]: Bytes
} Cell;                   // Strictly 1,088 bytes

// Pure deterministic reducer: returns leftover fuel (>0) on clean HALT, or 0 on TRAP
static uint16_t step_branch(const Cell *fn, const Cell *in, uint16_t fuel, Cell *out) {
    memset(out, 0, sizeof(Cell));
    uint8_t stack[64] = {0};
    int sp = 0, pc = 2, wp = 2;

    while (pc < 1024 && fn->body[pc] != 0x00) {
        if (fuel-- == 0) return 0; // OutOfFuel -> Branch dies
        uint8_t op = fn->body[pc++];
        if      (op == 0x01 && sp < 64 && pc < 1024) stack[sp++] = fn->body[pc++];
        else if (op == 0x02 && sp > 0)               stack[sp-1] = in->body[2 + (stack[sp-1] & 1021)];
        else if (op == 0x03 && sp > 1)             { stack[sp-2] += stack[sp-1]; sp--; }
        else if (op == 0x04 && sp > 1)             { stack[sp-2] ^= stack[sp-1]; sp--; }
        else if (op == 0x05 && sp > 0 && wp < 1024)  out->body[wp++] = stack[--sp];
        else if (op == 0x06 && sp > 0)             { if (!stack[--sp]) return 0; } // Failed ASSERT -> Branch dies
        else return 0; // Bad opcode -> Branch dies
    }
    *(uint16_t *)out->body = fuel; // Stamp fitness score (leftover fuel) into output header
    return fuel;
}

// Inside flow(), when active->left resolves to a Mitosis Fork Cell (*(uint16_t *)f->body == 0xFFFF):
static void resolve_mitosis(int sock, struct sockaddr_in *peer, const Cell *fork_cell, const Cell *input, uint8_t root[32]) {
    int a_slot = resolve(fork_cell->left);
    if (a_slot < 0) { sendto(sock, fork_cell->left, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
    int b_slot = resolve(fork_cell->right);
    if (b_slot < 0) { sendto(sock, fork_cell->right, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }

    Cell out_a, out_b;
    uint16_t fit_a = step_branch(&arena[a_slot], input, 512, &out_a);
    uint16_t fit_b = step_branch(&arena[b_slot], input, 512, &out_b);

    if (fit_a == 0 && fit_b == 0) return; // Both mutants died; trigger next synthesis round

    // Natural selection: higher leftover fuel wins; hash breaks exact ties
    uint8_t ha[32], hb[32];
    hash_cell(&out_a, ha);
    hash_cell(&out_b, hb);
    Cell *winner = (fit_a > fit_b || (fit_a == fit_b && memcmp(ha, hb, 32) > 0)) ? &out_a : &out_b;

    // Preserve winning operator lineage in left link & causal root in right link
    memcpy(winner->left,  (winner == &out_a) ? fork_cell->left : fork_cell->right, 32);
    memcpy(winner->right, root, 32);
    ingest(winner, root);
    sendto(sock, winner, sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
}
Use code with caution.

What Emerges From This Loop

• Zero-Downtime Speculation. Bad mutations (fit == 0) evaporate in microseconds without ever touching root or broadcasting onto UDP.
• Monotonic Optimization. Because winner->left preserves the winning operator's 32-byte hash, the system automatically carries forward whichever bytecode variant burned fewer instructions, turning the entire P2P network into a continuous, self-optimizing evolutionary engine.
Would you like to design the bytecode mutator (synth_hash) next so it performs crossover and opcode perturbation on two surviving parent Cells—turning simple A/B mitosis into full genetic synthesis inside the 1,024-byte body?
