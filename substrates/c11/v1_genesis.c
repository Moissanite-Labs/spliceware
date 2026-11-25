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

// ─── 1. UNIVERSAL 1,088-BYTE CELL (RAM == WIRE == HASH PRE-IMAGE) ─────────────
typedef struct __attribute__((packed)) {
    uint8_t left[32];     // Operator / Causal Parent / Mitosis Variant A
    uint8_t right[32];    // Operand  / Input Payload / Mitosis Variant B
    uint8_t body[1024];   // [0..1]: Fuel (0xFFFF == Fork) | [2..1023]: Bytes
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

// ─── 3. DETERMINISTIC STACK REDUCER (FUEL-METERED) ───────────────────────────
static uint16_t step_branch(const Cell *fn, const Cell *in, uint16_t fuel, Cell *out) {
    memset(out, 0, sizeof(Cell));
    uint8_t stack[64] = {0};
    int sp = 0, pc = 2, wp = 2;

    while (pc < 1024 && fn->body[pc] != 0x00) {
        if (fuel-- == 0) return 0;
        uint8_t op = fn->body[pc++];
        if      (op == 0x01 && sp < 64 && pc < 1024) stack[sp++] = fn->body[pc++];
        else if (op == 0x02 && sp > 0)               stack[sp-1] = in->body[2 + (stack[sp-1] & 1021)];
        else if (op == 0x03 && sp > 1)             { stack[sp-2] += stack[sp-1]; sp--; }
        else if (op == 0x04 && sp > 1)             { stack[sp-2] ^= stack[sp-1]; sp--; }
        else if (op == 0x05 && sp > 0 && wp < 1024)  out->body[wp++] = stack[--sp];
        else if (op == 0x06 && sp > 0)             { if (!stack[--sp]) return 0; }
        else return 0;
    }
    set_body_u16(out, fuel);
    return fuel;
}

// ─── 4. SACRED SPLICER (STACK-BALANCED RECOMBINATION & TRANSMUTATION) ─────────
static inline int codon_meta(uint8_t op, int *req_sp, int *d_sp) {
    switch (op) {
        case 0x01: *req_sp = 0; *d_sp = +1; return 2;
        case 0x02: *req_sp = 1; *d_sp =  0; return 1;
        case 0x03:
        case 0x04: *req_sp = 2; *d_sp = -1; return 1;
        case 0x05:
        case 0x06: *req_sp = 1; *d_sp = -1; return 1;
        default:   *req_sp = 0; *d_sp =  0; return 0;
    }
}

static int map_seams(const uint8_t body[1024], uint16_t off[513], uint8_t depth[513]) {
    int pc = 2, sp = 0, n = 0;
    while (pc < 1024 && body[pc] != 0x00 && n < 512) {
        int req, dsp, len = codon_meta(body[pc], &req, &dsp);
        if (len == 0 || pc + len > 1024 || sp < req || sp + dsp > 64) break;
        off[n] = (uint16_t)pc; depth[n] = (uint8_t)sp;
        sp += dsp; pc += len; n++;
    }
    off[n] = (uint16_t)pc; depth[n] = (uint8_t)sp;
    return n;
}

static int sacred_splice(const Cell *pa, const Cell *pb, const uint8_t seed[32], uint8_t out_fork[32]) {
    uint16_t oa[513], ob[513];
    uint8_t  da[513], db[513];
    int na = map_seams(pa->body, oa, da);
    int nb = map_seams(pb->body, ob, db);

    Cell ca = {0}, cb = {0};
    set_body_u16(&ca, 512);
    set_body_u16(&cb, 512);

    int cut_a = (na > 0) ? (seed[0] % (na + 1)) : 0, cut_b = -1;
    for (int i = 0; i <= nb; i++) {
        int probe = (seed[1] + i) % (nb + 1);
        if (db[probe] == da[cut_a]) { cut_b = probe; break; }
    }
    int head_len = oa[cut_a] - 2;
    memcpy(&ca.body[2], &pa->body[2], (size_t)head_len);
    if (cut_b >= 0) {
        int tail_len = ob[nb] - ob[cut_b];
        if (2 + head_len + tail_len < 1023)
            memcpy(&ca.body[2 + head_len], &pb->body[ob[cut_b]], (size_t)tail_len);
    }

    memcpy(&cb.body[2], &pa->body[2], (size_t)(oa[na] - 2));
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

// ─── 5. CONTINUOUS FLOW LOOP (UDP WIRE <-> MITOSIS <-> LINEAGE) ──────────────
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
        if (step_branch(fn, in, get_body_u16(active), &out) > 0) {
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
    if (c_slot < 0) {
        if (!is_zero(champ)) sendto(sock, champ, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
        return;
    }
    if (b_slot < 0) {
        if (!is_zero(ancestor_b)) sendto(sock, ancestor_b, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
        return;
    }
    uint8_t fork_h[32], seed[32];
    hash_cell(active, seed);
    if (sacred_splice(&arena[c_slot], &arena[b_slot], seed, fork_h) < 0) return;

    Cell next_gen = {0};
    memcpy(next_gen.left, fork_h, 32);
    memcpy(next_gen.right, active->right, 32);
    set_body_u16(&next_gen, 512);
    (void)ingest(&next_gen, root);
}

#ifndef SPLICEWARE_TEST_HARNESS
// ─── 6. SELF-BOOTSTRAPPING HARNESS ───────────────────────────────────────────
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
    memcpy(&a.body[2], code_a, sizeof(code_a));
    if (ingest(&a, ancestor_a) < 0) { close(sock); return 1; }
    // Seed Ancestor B (leaner strand: PUSH 0, LOAD, EMIT, HALT)
    Cell b = {0}; set_body_u16(&b, 512);
    uint8_t code_b[] = {0x01, 0x00, 0x02, 0x05, 0x00};
    memcpy(&b.body[2], code_b, sizeof(code_b));
    if (ingest(&b, ancestor_b) < 0) { close(sock); return 1; }
    uint8_t champ[32], root[32];
    memcpy(champ, ancestor_a, 32);
    printf("[NODE :%u <-> :%u] Igniting continuous evolutionary flow...\n", my_port, peer_port);
    for (int tick = 0; tick < 8; tick++) {
        // Feed live input byte into Arena & bind to current Champion
        Cell in = {0}; in.body[2] = (uint8_t)(0x41 + tick);
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
                   get_body_u16(res), res->body[2], res->body[2]);
        } else {
            printf("Tick %d | Champ=%02x%02x%02x%02x | <root unresolved>\n",
                   tick, champ[0], champ[1], champ[2], champ[3]);
        }
        struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    close(sock);
    return 0;
}
#endif
