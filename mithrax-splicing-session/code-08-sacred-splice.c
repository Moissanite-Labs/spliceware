// Returns exact instruction byte length (codon stride) and stack delta
static inline int codon_meta(uint8_t op, int *req_sp, int *d_sp) {
    switch (op) {
        case 0x01: *req_sp = 0; *d_sp = +1; return 2; // PUSH imm (2-byte codon)
        case 0x02: *req_sp = 1; *d_sp =  0; return 1; // LOAD
        case 0x03:                                    // ADD
        case 0x04: *req_sp = 2; *d_sp = -1; return 1; // XOR
        case 0x05:                                    // EMIT
        case 0x06: *req_sp = 1; *d_sp = -1; return 1; // ASSERT
        default:   *req_sp = 0; *d_sp =  0; return 0; // HALT (0x00) or invalid
    }
}

// Indexing pass: maps every valid codon boundary and its exact live stack depth
static int map_seams(const uint8_t *body, uint16_t off[512], uint8_t depth[512]) {
    int pc = 2, sp = 0, n = 0;
    while (pc < 1024 && body[pc] != 0x00 && n < 512) {
        int req, dsp;
        int len = codon_meta(body[pc], &req, &dsp);
        if (len == 0 || pc + len > 1024 || sp < req || sp + dsp > 64) break;
        off[n]   = (uint16_t)pc;
        depth[n] = (uint8_t)sp;
        sp += dsp;
        pc += len;
        n++;
    }
    off[n] = (uint16_t)pc; depth[n] = (uint8_t)sp; // Terminal seam before HALT
    return n;
}

// Sacred Splicer: emits a Mitosis Fork Cell [Child_Cross | Child_Transmute | 0xFFFF]
static void sacred_splice(const Cell *pa, const Cell *pb, const uint8_t seed[32], uint8_t out_fork[32]) {
    uint16_t oa[513], ob[513];
    uint8_t  da[513], db[513];
    int na = map_seams(pa->body, oa, da);
    int nb = map_seams(pb->body, ob, db);

    Cell ca = {0}, cb = {0};
    *(uint16_t *)ca.body = 512;
    *(uint16_t *)cb.body = 512;

    // 1. CHILD A: Stack-Matched Homologous Crossover (Head of A + Tail of B at equal sp)
    int cut_a = (na > 0) ? (seed[0] % (na + 1)) : 0;
    int cut_b = -1;
    for (int i = 0; i <= nb; i++) {
        int probe = (seed[1] + i) % (nb + 1);
        if (db[probe] == da[cut_a]) { cut_b = probe; break; } // Exact stack-depth lock!
    }
    int head_len = oa[cut_a] - 2;
    memcpy(&ca.body[2], &pa->body[2], head_len);
    if (cut_b >= 0) {
        int tail_len = ob[nb] - ob[cut_b];
        if (2 + head_len + tail_len < 1023)
            memcpy(&ca.body[2 + head_len], &pb->body[ob[cut_b]], tail_len);
    }

    // 2. CHILD B: Valence-Preserving Transmutation (Mutate codon without breaking Δsp)
    memcpy(&cb.body[2], &pa->body[2], oa[na] - 2);
    if (na > 0) {
        int target = oa[seed[2] % na];
        uint8_t op = cb.body[target];
        if      (op == 0x01) cb.body[target + 1] ^= (seed[3] ? seed[3] : 1); // Perturb immediate constant
        else if (op == 0x03) cb.body[target] = 0x04;                         // ADD <-> XOR (both Δsp = -1)
        else if (op == 0x04) cb.body[target] = 0x03;                         // XOR <-> ADD (both Δsp = -1)
        else if (op == 0x02 && oa[na] + 2 < 1022) {
            // Neutral Intron Insertion: splice [PUSH k, XOR] (net Δsp = +1 - 1 = 0) after LOAD
            memmove(&cb.body[target + 4], &cb.body[target + 1], oa[na] - (target + 1));
            cb.body[target + 1] = 0x01;
            cb.body[target + 2] = seed[4];
            cb.body[target + 3] = 0x04;
        }
    }

    // 3. BIND INTO MITOSIS FORK CELL (0xFFFF)
    Cell fork_cell = {0};
    *(uint16_t *)fork_cell.body = 0xFFFF;
    ingest(&ca, fork_cell.left);
    ingest(&cb, fork_cell.right);
    ingest(&fork_cell, out_fork);
}
