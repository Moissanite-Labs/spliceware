#define HDR_WEIGHTS 2   // body[2..11]: 10 deterministic adaptive codon weights
#define CODE_START  12  // body[12..1023]: executable bytecode

// Op table: 01:PUSH 02:LOAD 03:ADD 04:XOR 05:EMIT 06:ASSERT 07:LOOP 08:END 09:CALL
static inline int codon_meta(uint8_t op, int *req_sp, int *d_sp) {
    switch (op) {
        case 0x01: *req_sp = 0; *d_sp = +1; return 2; // PUSH imm
        case 0x02: *req_sp = 1; *d_sp =  0; return 1; // LOAD [in]
        case 0x03:
        case 0x04: *req_sp = 2; *d_sp = -1; return 1; // ADD / XOR
        case 0x05:
        case 0x06: *req_sp = 1; *d_sp = -1; return 1; // EMIT / ASSERT
        case 0x07: *req_sp = 0; *d_sp =  0; return 1; // LOOP (mark)
        case 0x08: *req_sp = 1; *d_sp = -1; return 1; // END  (pop cond, branch if !=0)
        case 0x09: *req_sp = 1; *d_sp =  0; return 1; // CALL (sub-cell transform)
        default:   *req_sp = 0; *d_sp =  0; return 0;
    }
}

// Load-time invariant check: verifies stack bounds AND balanced, zero-delta LOOP..END
static int verify_and_map(const uint8_t *body, uint16_t *off, uint8_t *depth) {
    int pc = CODE_START, sp = 0, csp = 0, n = 0;
    int loop_sp[16];
    while (pc < 1024 && body[pc] != 0x00 && n < 512) {
        uint8_t op = body[pc];
        int req, dsp, len = codon_meta(op, &req, &dsp);
        if (len == 0 || pc + len > 1024 || sp < req || sp + dsp > 64) return -1;

        if (op == 0x07) {
            if (csp >= 16) return -1;
            loop_sp[csp++] = sp; // Record stack depth at LOOP entry
        } else if (op == 0x08) {
            if (csp <= 0 || (sp + dsp) != loop_sp[--csp]) return -1; // Enforce Δsp == 0!
        }
        // Only expose top-level (csp == 0) seams so splices never rip a LOOP in half
        if (csp == 0) { off[n] = (uint16_t)pc; depth[n] = (uint8_t)sp; n++; }
        sp += dsp; pc += len;
    }
    if (csp != 0) return -1; // Unclosed LOOP rejected at load time
    off[n] = (uint16_t)pc; depth[n] = (uint8_t)sp;
    return n;
}

// Deterministic fuel-metered reducer with bounded LOOP & content-addressed CALL
static uint16_t step_branch(const Cell *fn, const Cell *in, uint16_t fuel, Cell *out) {
    uint16_t dummy_o[513]; uint8_t dummy_d[513];
    if (verify_and_map(fn->body, dummy_o, dummy_d) < 0) return 0; // Gate untrusted Cell

    memset(out, 0, sizeof(Cell));
    memcpy(&out->body[HDR_WEIGHTS], &fn->body[HDR_WEIGHTS], 10); // Inherit prior
    uint8_t stack[64] = {0};
    uint16_t cstack[16] = {0};
    int sp = 0, csp = 0, pc = CODE_START, wp = CODE_START;

    while (pc < 1024 && fn->body[pc] != 0x00) {
        if (fuel-- == 0) return 0;
        uint8_t op = fn->body[pc++];
        if      (op == 0x01) stack[sp++] = fn->body[pc++];
        else if (op == 0x02) stack[sp-1] = in->body[CODE_START + (stack[sp-1] & 1011)];
        else if (op == 0x03) { stack[sp-2] += stack[sp-1]; sp--; }
        else if (op == 0x04) { stack[sp-2] ^= stack[sp-1]; sp--; }
        else if (op == 0x05 && wp < 1024) out->body[wp++] = stack[--sp];
        else if (op == 0x06) { if (!stack[--sp]) return 0; }
        else if (op == 0x07) { cstack[csp++] = (uint16_t)pc; }
        else if (op == 0x08) {
            if (stack[--sp] != 0) {
                uint16_t penalty = (uint16_t)csp; // Deeper loops cost extra fuel
                if (fuel < penalty) return 0;
                fuel -= penalty;
                pc = cstack[csp - 1];             // Deterministic backward jump
            } else csp--;
        }
        else if (op == 0x09) { // CALL fn->left on a synthetic 1-byte input Cell
            int callee = resolve(fn->left);
            if (callee < 0 || fuel < 4) return 0;
            Cell sub_in = {0}, sub_out = {0};
            sub_in.body[CODE_START] = stack[sp - 1];
            uint16_t rem = step_branch(&arena[callee], &sub_in, fuel >> 1, &sub_out);
            if (rem == 0) return 0;
            fuel -= ((fuel >> 1) - rem);
            stack[sp - 1] = sub_out.body[CODE_START];
        }
    }
    *(uint16_t *)out->body = fuel;
    return fuel;
}

// Deterministic CDF sampler driven purely by ARX hash byte + Cell's live prior
static uint8_t sample_codon(const uint8_t weights[10], uint8_t entropy_byte) {
    uint32_t total = 0;
    for (int i = 1; i <= 9; i++) total += (weights[i] ? weights[i] : 16);
    uint32_t target = ((uint32_t)entropy_byte * total) >> 8, acc = 0;
    for (int i = 1; i <= 9; i++) {
        acc += (weights[i] ? weights[i] : 16);
        if (target < acc) return (uint8_t)i;
    }
    return 0x04;
}

// Deterministic Multiplicative-Weights update on tournament victory
static void adapt_prior(Cell *winner_fn, uint16_t fuel_margin) {
    uint8_t *w = &winner_fn->body[HDR_WEIGHTS];
    uint8_t seen[10] = {0};
    for (int pc = CODE_START; pc < 1024 && winner_fn->body[pc] != 0x00; ) {
        uint8_t op = winner_fn->body[pc];
        int req, dsp, len = codon_meta(op, &req, &dsp);
        if (len <= 0) break;
        if (op <= 9) seen[op] = 1;
        pc += len;
    }
    uint8_t boost = (fuel_margin > 15) ? 16 : (uint8_t)(fuel_margin + 1);
    for (int i = 1; i <= 9; i++) {
        uint16_t cur = w[i] ? w[i] : 16;
        cur = seen[i] ? (cur + boost) : (cur - (cur >> 3));
        w[i] = (uint8_t)(cur > 255 ? 255 : (cur < 2 ? 2 : cur));
    }
}
