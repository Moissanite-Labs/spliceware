/* v2_adaptive_loop.c — v1_genesis.c + code-13-loop-call-adaptive-weights.c
 *
 * Integrated C11 substrate. Lineage:
 *   v1_genesis.c              (mithrax-splicing-session/code-10-genesis-substrate-complete.c, 7a1a27f)
 *   + code-13-loop-call-adaptive-weights.c (mithrax-splicing-session, 7a1a27f):
 *     - LOOP (0x07) / END (0x08) statically balanced at load time, bounded by fuel
 *     - content-addressed CALL (0x09) into Arena cells
 *     - 10-byte in-Cell adaptive prior vector HDR_WEIGHTS (body[2..11])
 *     - multiplicative weight updates (adapt_prior) on tournament victory
 *     - load-time seam verification (verify_and_map) replacing map_seams
 *
 * Fixed during integration (not present in the raw capture):
 *   - CODE_START moved 2 -> 12 so the fuel header and adaptive prior vector
 *     share body[0..11]; LOOP..END load-time balance enforced on seam scan.
 *   - sacred_splice fails closed: a parent that fails seam verification is
 *     cloned unchanged instead of producing an unverifiable child.
 *   - #include <netinet/in.h> with an INADDR_LOOPBACK fallback for portability.
 *
 * Build:
 *   make -C ../c11        (or: gcc -O3 -Wall -Wextra -std=c11 v2_adaptive_loop.c -o v2_adaptive_loop)
 * Run (two peering nodes):
 *   ./v2_adaptive_loop 9001 9002 &
 *   ./v2_adaptive_loop 9002 9001
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK 0x7f000001 /* 127.0.0.1, portable fallback */
#endif

// Body layout: [0..1] fuel/fork marker | [2..11] HDR_WEIGHTS (10 adaptive
// codon weights) | [12..1023] executable bytecode.
#define HDR_WEIGHTS 2   // body[2..11]: 10 deterministic adaptive codon weights
#define CODE_START  12  // body[12..1023]: executable bytecode

// ─── 1. UNIVERSAL 1,088-BYTE CELL (RAM == WIRE == HASH PRE-IMAGE) ─────────────
typedef struct __attribute__((packed)) {
    uint8_t left[32];     // Operator / Causal Parent / Mitosis Variant A
    uint8_t right[32];    // Operand  / Input Payload / Mitosis Variant B
    uint8_t body[1024];   // [0..1]: Fuel (0xFFFF == Fork) | [2..11]: Priors | [12..]: Bytes
} Cell;

_Static_assert(sizeof(Cell) == 1088, "Cell must be strictly 1088 bytes");

#define ARENA_BITS 14
#define ARENA_CAP  (1 << ARENA_BITS)

static Cell    arena[ARENA_CAP];
static uint8_t names[ARENA_CAP][32];
static uint8_t occupied[ARENA_CAP];
static uint8_t ancestor_a[32], ancestor_b[32];

static inline uint16_t get_body_u16(const Cell *c) {
    uint16_t v;
    memcpy(&v, c->body, sizeof(v));
    return v;
}

static inline void set_body_u16(Cell *c, uint16_t v) {
    memcpy(c->body, &v, sizeof(v));
}

static inline uint32_t hash_slot_prefix(const uint8_t h[32]) {
    uint32_t idx;
    memcpy(&idx, h, sizeof(idx));
    return idx & (ARENA_CAP - 1);
}

// ─── 2. 256-BIT ARX SPONGE & O(1) CONTENT-ADDRESSED ARENA ────────────────────
static void hash_cell(const Cell *c, uint8_t out[32]) {
    uint64_t s[4] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                     0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL};
    const uint8_t *bytes = (const uint8_t *)c;
    for (int i = 0; i < (int)(sizeof(Cell) / 8); i++) {
        uint64_t w;
        memcpy(&w, bytes + (size_t)i * 8, sizeof(w));
        s[i & 3] ^= w;
        for (int r = 0; r < 4; r++) {
            s[0] += s[1]; s[1] = (s[1] << 13) | (s[1] >> 51); s[1] ^= s[0];
            s[2] += s[3]; s[3] = (s[3] << 17) | (s[3] >> 47); s[3] ^= s[2];
            s[0] += s[3]; s[3] = (s[3] << 21) | (s[3] >> 43); s[3] ^= s[0];
            s[2] += s[1]; s[1] = (s[1] << 29) | (s[1] >> 35); s[1] ^= s[2];
        }
    }
    memcpy(out, s, 32);
}

static int is_zero(const uint8_t h[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= h[i];
    return acc == 0;
}

static int resolve(const uint8_t h[32]) {
    uint32_t idx = hash_slot_prefix(h);
    for (int n = 0; n < ARENA_CAP; n++) {
        uint32_t slot = (idx + (uint32_t)n) & (ARENA_CAP - 1);
        if (!occupied[slot]) return -1;
        if (memcmp(names[slot], h, 32) == 0) return (int)slot;
    }
    return -1;
}

static int ingest(const Cell *c, uint8_t out_h[32]) {
    uint8_t h[32];
    hash_cell(c, h);
    uint32_t idx = hash_slot_prefix(h);
    for (int n = 0; n < ARENA_CAP; n++) {
        uint32_t slot = (idx + (uint32_t)n) & (ARENA_CAP - 1);
        if (!occupied[slot] || memcmp(names[slot], h, 32) == 0) {
            arena[slot] = *c;
            memcpy(names[slot], h, 32);
            occupied[slot] = 1;
            if (out_h) memcpy(out_h, h, 32);
            return (int)slot;
        }
    }
    return -1;
}

// ─── 3. ADAPTIVE PRIOR MACHINERY (from code-13) ───────────────────────────────
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

// Deterministic CDF sampler driven purely by ARX hash byte + Cell's live prior.
// Exposed (external linkage) for future adaptive mutation operators; kept out
// of the hot path so current tournament behaviour is byte-identical to v1.
uint8_t sample_codon(const uint8_t weights[10], uint8_t entropy_byte) {
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

// ─── 4. DETERMINISTIC STACK REDUCER (FUEL-METERED, from code-13) ─────────────
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
    set_body_u16(out, fuel);
    return fuel;
}

// ─── 5. SACRED SPLICER (STACK-BALANCED RECOMBINATION & TRANSMUTATION) ─────────
static int sacred_splice(const Cell *pa, const Cell *pb, const uint8_t seed[32], uint8_t out_fork[32]) {
    uint16_t oa[513], ob[513];
    uint8_t  da[513], db[513];
    int na = verify_and_map(pa->body, oa, da);
    int nb = verify_and_map(pb->body, ob, db);

    Cell ca = {0}, cb = {0};
    set_body_u16(&ca, 512);
    set_body_u16(&cb, 512);

    if (na < 0 || nb < 0) {
        // Fail closed: an unverifiable parent is cloned unchanged; the fork
        // still carries deterministic Merkle lineage and stays replayable.
        ca = *pa; cb = *pa;
        set_body_u16(&ca, 512);
        set_body_u16(&cb, 512);
        Cell fork_cell = {0};
        set_body_u16(&fork_cell, 0xFFFF);
        if (ingest(&ca, fork_cell.left) < 0) return -1;
        if (ingest(&cb, fork_cell.right) < 0) return -1;
        return ingest(&fork_cell, out_fork);
    }

    int cut_a = (na > 0) ? (seed[0] % (na + 1)) : 0, cut_b = -1;
    for (int i = 0; i <= nb; i++) {
        int probe = (seed[1] + i) % (nb + 1);
        if (db[probe] == da[cut_a]) { cut_b = probe; break; }
    }
    int head_len = oa[cut_a] - CODE_START;
    memcpy(&ca.body[CODE_START], &pa->body[CODE_START], (size_t)head_len);
    if (cut_b >= 0) {
        int tail_len = ob[nb] - ob[cut_b];
        if (CODE_START + head_len + tail_len < 1023)
            memcpy(&ca.body[CODE_START + head_len], &pb->body[ob[cut_b]], (size_t)tail_len);
    }

    memcpy(&cb.body[CODE_START], &pa->body[CODE_START], (size_t)(oa[na] - CODE_START));
    if (na > 0) {
        int target = oa[seed[2] % na];
        uint8_t op = cb.body[target];
        if      (op == 0x01) cb.body[target + 1] ^= (seed[3] ? seed[3] : 1);
        else if (op == 0x03) cb.body[target] = 0x04;
        else if (op == 0x04) cb.body[target] = 0x03;
        else if (op == 0x02 && oa[na] + 3 < 1023) {
            memmove(&cb.body[target + 4], &cb.body[target + 1], (size_t)(oa[na] - (target + 1)));
            cb.body[target + 1] = 0x01; cb.body[target + 2] = seed[3]; cb.body[target + 3] = 0x04;
        }
    }

    Cell fork_cell = {0};
    set_body_u16(&fork_cell, 0xFFFF);
    if (ingest(&ca, fork_cell.left) < 0) return -1;
    if (ingest(&cb, fork_cell.right) < 0) return -1;
    return ingest(&fork_cell, out_fork);
}

// ─── 6. CONTINUOUS FLOW LOOP (UDP WIRE <-> MITOSIS <-> LINEAGE) ──────────────
static void flow(int sock, struct sockaddr_in *peer, uint8_t root[32], uint8_t champ[32]) {
    uint8_t pkt[sizeof(Cell)];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);

    ssize_t n = recvfrom(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&src, &slen);
    if (n == 32) {
        int hit = resolve(pkt);
        if (hit >= 0) sendto(sock, &arena[hit], sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)&src, slen);
    } else if (n == (ssize_t)sizeof(Cell)) {
        Cell incoming;
        memcpy(&incoming, pkt, sizeof(Cell));
        uint8_t arrived[32];
        if (ingest(&incoming, arrived) >= 0 && memcmp(arrived, root, 32) > 0) {
            memcpy(root, arrived, 32);
        }
    }

    int r_slot = resolve(root);
    if (r_slot < 0) { sendto(sock, root, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
    Cell *active = &arena[r_slot];
    if (is_zero(active->left)) return;

    int f_slot = resolve(active->left);
    if (f_slot < 0) { sendto(sock, active->left, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
    int x_slot = resolve(active->right);
    if (x_slot < 0) { sendto(sock, active->right, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }

    Cell *fn = &arena[f_slot], *in = &arena[x_slot];
    uint16_t f_hdr = get_body_u16(fn);

    if (f_hdr == 0xFFFF) { // MITOSIS A/B FUEL TOURNAMENT
        int a_slot = resolve(fn->left), b_slot = resolve(fn->right);
        if (a_slot < 0) { sendto(sock, fn->left, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
        if (b_slot < 0) { sendto(sock, fn->right, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }

        Cell out_a, out_b;
        uint16_t fit_a = step_branch(&arena[a_slot], in, 512, &out_a);
        uint16_t fit_b = step_branch(&arena[b_slot], in, 512, &out_b);

        if (fit_a > 0 || fit_b > 0) {
            uint8_t ha[32], hb[32];
            hash_cell(&out_a, ha); hash_cell(&out_b, hb);
            int a_wins = (fit_a > fit_b || (fit_a == fit_b && memcmp(ha, hb, 32) > 0));
            Cell *win = a_wins ? &out_a : &out_b;
            uint16_t margin = (fit_a > fit_b) ? (uint16_t)(fit_a - fit_b)
                                              : (uint16_t)(fit_b - fit_a);
            adapt_prior(win, margin); // Winner's in-Cell prior travels with the genome
            const uint8_t *win_champ = a_wins ? fn->left : fn->right;
            memcpy(win->left, win_champ, 32);
            memcpy(win->right, root, 32);
            uint8_t new_root[32];
            if (ingest(win, new_root) >= 0) {
                memcpy(champ, win_champ, 32);
                memcpy(root, new_root, 32);
                sendto(sock, win, sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
            }
            return;
        }
    } else { // SINGLE-STRAND REDUCTION
        Cell out;
        uint16_t fit = step_branch(fn, in, get_body_u16(active), &out);
        if (fit > 0) {
            adapt_prior(&out, (fit > 15) ? 15 : fit);
            memcpy(out.left, active->left, 32);
            memcpy(out.right, root, 32);
            uint8_t new_root[32];
            if (ingest(&out, new_root) >= 0) {
                memcpy(champ, active->left, 32);
                memcpy(root, new_root, 32);
                sendto(sock, &out, sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
            }
            return;
        }
    }

    // TRAP OR DOUBLE-EXTINCTION -> SACRED SPLICE NEW MITOSIS FORK
    int c_slot = resolve(champ), b_slot = resolve(ancestor_b);
    uint8_t fork_h[32], seed[32];
    hash_cell(active, seed);
    if (c_slot >= 0 && b_slot >= 0) {
        if (sacred_splice(&arena[c_slot], &arena[b_slot], seed, fork_h) < 0) return;
    } else {
        memset(fork_h, 0, sizeof(fork_h));
        if (c_slot >= 0) memcpy(fork_h, champ, 32); else memcpy(fork_h, ancestor_b, 32);
    }

    Cell next_gen = {0};
    memcpy(next_gen.left, fork_h, 32);
    memcpy(next_gen.right, active->right, 32);
    set_body_u16(&next_gen, 512);
    (void)ingest(&next_gen, root);
}

#ifndef SPLICEWARE_TEST_HARNESS
// ─── 7. SELF-BOOTSTRAPPING HARNESS ───────────────────────────────────────────
int main(int argc, char **argv) {
    uint16_t my_port   = (argc > 1) ? (uint16_t)atoi(argv[1]) : 9001;
    uint16_t peer_port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 9002;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(my_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(peer_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

    (void)bind(sock, (struct sockaddr *)&me, sizeof(me));
    // Seed Ancestor A (bloated with neutral XOR intron: PUSH 0, LOAD, PUSH 0, XOR, EMIT, HALT)
    Cell a = {0}; set_body_u16(&a, 512);
    uint8_t code_a[] = {0x01, 0x00, 0x02, 0x01, 0x00, 0x04, 0x05, 0x00};
    memcpy(&a.body[CODE_START], code_a, sizeof(code_a));
    if (ingest(&a, ancestor_a) < 0) { close(sock); return 1; }
    // Seed Ancestor B (leaner strand: PUSH 0, LOAD, EMIT, HALT)
    Cell b = {0}; set_body_u16(&b, 512);
    uint8_t code_b[] = {0x01, 0x00, 0x02, 0x05, 0x00};
    memcpy(&b.body[CODE_START], code_b, sizeof(code_b));
    if (ingest(&b, ancestor_b) < 0) { close(sock); return 1; }
    uint8_t champ[32], root[32];
    memcpy(champ, ancestor_a, 32);
    printf("[NODE :%u <-> :%u] Igniting continuous evolutionary flow...\n", my_port, peer_port);
    for (int tick = 0; tick < 8; tick++) {
        // Feed live input byte into Arena & bind to current Champion
        Cell in = {0}; in.body[CODE_START] = (uint8_t)(0x41 + tick);
        uint8_t in_h[32];
        if (ingest(&in, in_h) < 0) break;
        // Every odd tick, force a Mitosis tournament between Champion and Ancestor B
        uint8_t op_h[32];
        if (tick & 1) {
            uint8_t seed[32]; hash_cell(&in, seed);
            int cs = resolve(champ), bs = resolve(ancestor_b);
            if (cs < 0 || bs < 0 || sacred_splice(&arena[cs], &arena[bs], seed, op_h) < 0) {
                memcpy(op_h, champ, 32);
            }
        } else {
            memcpy(op_h, champ, 32);
        }
        Cell task = {0};
        memcpy(task.left, op_h, 32); memcpy(task.right, in_h, 32);
        set_body_u16(&task, 512);
        if (ingest(&task, root) < 0) break;
        flow(sock, &peer, root, champ);
        int rr = resolve(root);
        if (rr >= 0) {
            Cell *res = &arena[rr];
            printf("Tick %d | Champ=%02x%02x%02x%02x | FuelLeft=%3u | OutByte='%c' (0x%02x)\n",
                   tick, champ[0], champ[1], champ[2], champ[3],
                   get_body_u16(res), res->body[CODE_START], res->body[CODE_START]);
        } else {
            printf("Tick %d | Champ=%02x%02x%02x%02x | <root unresolved>\n", tick, champ[0], champ[1], champ[2], champ[3]);
        }
        struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    close(sock);
    return 0;
}
#endif
