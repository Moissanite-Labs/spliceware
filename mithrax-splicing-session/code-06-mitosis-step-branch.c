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
