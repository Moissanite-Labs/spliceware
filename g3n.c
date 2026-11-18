/* genesis.c — Self-Bootstrapping Content-Addressed Evolutionary Substrate
 * Architecture:
 *   - 1,088-byte packed Cell (64-byte Merkle-ARX header + 1,024-byte body)
 *   - 256-bit ARX Sponge & O(1) Content-Addressed Arena (RAM == WIRE == HASH)
 *   - Raw UDP Cache-Miss Gossip (32B hash requests, 1,088B Cell responses)
 *   - Deterministic LOOP/END + content-addressed CALL + DUP/OVER stack codons
 *   - Stack-balanced Mitosis crossover (sacred_splice)
 *   - In-Cell 12-byte multiplicative adaptive prior weights (adapt_prior)
 *   - 8-byte Triangular Sum two-tiered lexicase anchor gate
 *
 * Build:
 *   gcc -O3 -Wall -Wextra -std=c11 genesis.c -o genesis
 * Run (two peering nodes):
 *   ./genesis 9001 9002 &
 *   ./genesis 9002 9001
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ============================================================================
 * 1. THE 1,088-BYTE CELL & BODY LAYOUT
 * ============================================================================ */
#define CELL_BYTES   1088
#define BODY_SIZE    1024
#define ARENA_CAP    16384
#define ARENA_MASK   (ARENA_CAP - 1)

/* Body header layout (bytes 0..19 inside body[1024]):
 *   [0..1]   uint16_t code_len   : active bytecode length (or 0xFFFF for Mitosis fork)
 *   [2..13]  uint8_t  weights[12]: adaptive prior weights for codons 0x00..0x0B
 *   [14..21] uint8_t  anchors[8] : 4 x (uint8_t in, uint8_t out) Triangular Sum anchors
 *   [22..]   uint8_t  code[]     : executable bytecode payload
 */
#define HDR_WEIGHTS_OFF 2
#define NUM_CODONS      12
#define HDR_ANCHORS_OFF 14
#define NUM_ANCHORS     4
#define CODE_START      22
#define MAX_CODE_LEN    (BODY_SIZE - CODE_START)
#define FORK_MARKER     0xFFFFu

#define MAX_STACK       64
#define MAX_LOOPS       16
#define MAX_CALL_DEPTH  8
#define MAX_FUEL        512

typedef struct __attribute__((packed)) {
    uint8_t left[32];        /* 256-bit ARX content hash of left parent / sub-cell */
    uint8_t right[32];       /* 256-bit ARX content hash of right parent / peer */
    uint8_t body[BODY_SIZE]; /* Header metadata + bytecode payload */
} Cell;

_Static_assert(sizeof(Cell) == CELL_BYTES, "Cell must be strictly 1088 bytes");

/* ============================================================================
 * 2. 256-BIT ARX SPONGE & O(1) CONTENT-ADDRESSED ARENA
 * ============================================================================ */
static inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static void arx_permute(uint32_t s[8]) {
    for (int round = 0; round < 12; round++) {
        for (int i = 0; i < 4; i++) {
            uint32_t a = s[i], b = s[i + 4];
            a += b; b = rotl32(b ^ a, 7);
            a += b; b = rotl32(b ^ a, 13);
            a += b; b = rotl32(b ^ a, 19);
            s[i] = a;
            s[(i + 1) & 3] ^= (uint32_t)(round * 0x9E3779B9u);
            s[i + 4] = b;
        }
    }
}

static void arx_256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
    };
    const uint8_t *p = (const uint8_t *)data;
    size_t offset = 0;

    while (offset < len) {
        uint8_t block[16] = {0};
        size_t chunk = (len - offset >= 16) ? 16 : (len - offset);
        memcpy(block, p + offset, chunk);
        if (chunk < 16) block[chunk] = 0x80;

        for (int i = 0; i < 4; i++) {
            uint32_t w;
            memcpy(&w, block + i * 4, 4);
            state[i] ^= w;
        }
        arx_permute(state);
        offset += 16;
    }
    state[7] ^= (uint32_t)len;
    arx_permute(state);
    memcpy(out, state, 32);
}

static void hash_cell(const Cell *c, uint8_t out[32]) {
    arx_256(c, sizeof(Cell), out);
}

/* Open-addressed O(1) Content-Addressed Arena */
static Cell    arena[ARENA_CAP];
static uint8_t names[ARENA_CAP][32];
static uint8_t occupied[ARENA_CAP];

static inline uint32_t slot_from_hash(const uint8_t h[32]) {
    uint32_t idx;
    memcpy(&idx, h, 4);
    return idx & ARENA_MASK;
}

static int arena_lookup(const uint8_t h[32]) {
    uint32_t idx = slot_from_hash(h);
    for (uint32_t probe = 0; probe < ARENA_CAP; probe++) {
        uint32_t slot = (idx + probe) & ARENA_MASK;
        if (!occupied[slot]) return -1;
        if (memcmp(names[slot], h, 32) == 0) return (int)slot;
    }
    return -1;
}

static int arena_put(const Cell *c, uint8_t out_hash[32]) {
    uint8_t h[32];
    hash_cell(c, h);
    if (out_hash) memcpy(out_hash, h, 32);

    uint32_t idx = slot_from_hash(h);
    for (uint32_t probe = 0; probe < ARENA_CAP; probe++) {
        uint32_t slot = (idx + probe) & ARENA_MASK;
        if (!occupied[slot]) {
            arena[slot] = *c;
            memcpy(names[slot], h, 32);
            occupied[slot] = 1;
            return (int)slot;
        }
        if (memcmp(names[slot], h, 32) == 0) {
            return (int)slot; /* Already deduplicated in RAM */
        }
    }
    /* Evict base slot on full table */
    arena[idx] = *c;
    memcpy(names[idx], h, 32);
    occupied[idx] = 1;
    return (int)idx;
}

/* ============================================================================
 * 3. RAW UDP CACHE-MISS GOSSIP (32B HASH REQUEST <-> 1,088B CELL RESPONSE)
 * ============================================================================ */
static int udp_sock = -1;
static struct sockaddr_in peer_addr;
static bool has_peer = false;

static void request_missing_hash(const uint8_t h[32]) {
    if (!has_peer || udp_sock < 0) return;
    /* Don't request the zero hash */
    uint8_t zero[32] = {0};
    if (memcmp(h, zero, 32) == 0) return;
    sendto(udp_sock, h, 32, 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr));
}

static const Cell *arena_fetch_or_gossip(const uint8_t h[32]) {
    int slot = arena_lookup(h);
    if (slot >= 0) return &arena[slot];
    request_missing_hash(h);
    return NULL;
}

static void poll_network(uint8_t latest_gossip_hash[32], bool *got_new_cell) {
    if (udp_sock < 0) return;
    uint8_t buf[CELL_BYTES];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);

    while (1) {
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n <= 0) break;

        if (n == 32) {
            /* Cache-miss query from peer: reply with 1,088B Cell if present */
            int slot = arena_lookup(buf);
            if (slot >= 0) {
                sendto(udp_sock, &arena[slot], CELL_BYTES, 0, (struct sockaddr *)&src, slen);
            }
        } else if (n == CELL_BYTES) {
            /* Incoming 1,088B Cell: verify content hash implicitly via arena_put */
            Cell incoming;
            memcpy(&incoming, buf, CELL_BYTES);
            uint8_t h[32];
            bool existed = (arena_lookup(h) >= 0);
            arena_put(&incoming, h);
            if (!existed && latest_gossip_hash && got_new_cell) {
                memcpy(latest_gossip_hash, h, 32);
                *got_new_cell = true;
            }
        }
    }
}

/* ============================================================================
 * 4. DETERMINISTIC CODONS, STACK VERIFIER & BOUNDED VM (LOOP/END/CALL/DUP/OVER)
 * ============================================================================ */
enum {
    OP_NOP   = 0x00, /* sp += 0 */
    OP_IN    = 0x01, /* sp += 1 : push input x */
    OP_ONE   = 0x02, /* sp += 1 : push 1 */
    OP_ADD   = 0x03, /* sp -= 1 : a + b */
    OP_SUB   = 0x04, /* sp -= 1 : a - b */
    OP_MUL   = 0x05, /* sp -= 1 : a * b */
    OP_SHR   = 0x06, /* sp += 0 : a >> 1 */
    OP_LOOP  = 0x07, /* sp -= 1 : pop iteration count, enter loop */
    OP_END   = 0x08, /* sp += 0 : decrement loop counter or exit */
    OP_CALL  = 0x09, /* sp += 0 : invoke content-addressed cell at fn->left */
    OP_DUP   = 0x0A, /* sp += 1 : [..., a] -> [..., a, a] */
    OP_OVER  = 0x0B  /* sp += 1 : [..., a, b] -> [..., a, b, a] */
};

static inline uint16_t get_code_len(const Cell *c) {
    uint16_t len;
    memcpy(&len, c->body, 2);
    return len;
}

static inline void set_code_len(Cell *c, uint16_t len) {
    memcpy(c->body, &len, 2);
}

/* Verifies bracket balance (LOOP..END), enforces net stack delta == 0 across every
 * loop body, and records per-instruction stack depth & matching jump targets. */
static bool verify_and_map(const uint8_t *code, uint16_t len,
                           int depth_map[MAX_CODE_LEN + 1],
                           int jump_map[MAX_CODE_LEN]) {
    if (len > MAX_CODE_LEN) return false;

    int sp = 0;
    int loop_stack_pc[MAX_LOOPS];
    int loop_entry_sp[MAX_LOOPS];
    int lsp = 0;

    for (uint16_t pc = 0; pc < len; pc++) {
        depth_map[pc] = sp;
        jump_map[pc] = -1;
        uint8_t op = code[pc] % NUM_CODONS;

        switch (op) {
            case OP_NOP:
                break;
            case OP_IN:
            case OP_ONE:
                sp += 1;
                break;
            case OP_DUP:
                if (sp < 1) return false;
                sp += 1;
                break;
            case OP_OVER:
                if (sp < 2) return false;
                sp += 1;
                break;
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
                if (sp < 2) return false;
                sp -= 1;
                break;
            case OP_SHR:
                if (sp < 1) return false;
                break;
            case OP_CALL:
                if (sp < 1) return false;
                break;
            case OP_LOOP:
                if (sp < 1 || lsp >= MAX_LOOPS) return false;
                sp -= 1; /* pops loop count */
                loop_stack_pc[lsp] = pc;
                loop_entry_sp[lsp] = sp;
                lsp++;
                break;
            case OP_END: {
                if (lsp <= 0) return false;
                lsp--;
                int open_pc = loop_stack_pc[lsp];
                /* Strict invariant: loop body must leave stack depth unchanged */
                if (sp != loop_entry_sp[lsp]) return false;
                jump_map[open_pc] = pc;
                jump_map[pc] = open_pc;
                break;
            }
            default:
                return false;
        }
        if (sp < 0 || sp >= MAX_STACK) return false;
    }

    if (lsp != 0) return false;
    depth_map[len] = sp;
    return true;
}

static int32_t exec_cell(const Cell *fn, int32_t input_x, int depth, int *fuel) {
    if (!fn || depth > MAX_CALL_DEPTH || *fuel <= 0) return 0;

    uint16_t len = get_code_len(fn);
    if (len == FORK_MARKER || len == 0 || len > MAX_CODE_LEN) return 0;

    const uint8_t *code = fn->body + CODE_START;
    int depth_map[MAX_CODE_LEN + 1];
    int jump_map[MAX_CODE_LEN];
    if (!verify_and_map(code, len, depth_map, jump_map)) return 0;

    int32_t stack[MAX_STACK];
    int sp = 0;
    int32_t loop_rem[MAX_LOOPS];
    int lsp = 0;

    uint16_t pc = 0;
    while (pc < len && *fuel > 0) {
        (*fuel)--;
        uint8_t op = code[pc] % NUM_CODONS;

        switch (op) {
            case OP_NOP:
                pc++;
                break;
            case OP_IN:
                stack[sp++] = input_x;
                pc++;
                break;
            case OP_ONE:
                stack[sp++] = 1;
                pc++;
                break;
            case OP_DUP:
                stack[sp] = stack[sp - 1];
                sp++;
                pc++;
                break;
            case OP_OVER:
                stack[sp] = stack[sp - 2];
                sp++;
                pc++;
                break;
            case OP_ADD: {
                int32_t b = stack[--sp], a = stack[--sp];
                stack[sp++] = (int32_t)((uint32_t)a + (uint32_t)b);
                pc++;
                break;
            }
            case OP_SUB: {
                int32_t b = stack[--sp], a = stack[--sp];
                stack[sp++] = (int32_t)((uint32_t)a - (uint32_t)b);
                pc++;
                break;
            }
            case OP_MUL: {
                int32_t b = stack[--sp], a = stack[--sp];
                stack[sp++] = (int32_t)((uint32_t)a * (uint32_t)b);
                pc++;
                break;
            }
            case OP_SHR:
                stack[sp - 1] = (int32_t)((uint32_t)stack[sp - 1] >> 1);
                pc++;
                break;
            case OP_LOOP: {
                int32_t cnt = stack[--sp];
                if (cnt < 0) cnt = 0;
                if (cnt > 64) cnt = 64;
                if (cnt == 0) {
                    pc = (uint16_t)(jump_map[pc] + 1);
                } else {
                    loop_rem[lsp++] = cnt - 1;
                    pc++;
                }
                break;
            }
            case OP_END: {
                if (lsp > 0 && loop_rem[lsp - 1] > 0) {
                    loop_rem[lsp - 1]--;
                    pc = (uint16_t)(jump_map[pc] + 1);
                } else {
                    if (lsp > 0) lsp--;
                    pc++;
                }
                break;
            }
            case OP_CALL: {
                const Cell *callee = arena_fetch_or_gossip(fn->left);
                if (callee) {
                    stack[sp - 1] = exec_cell(callee, stack[sp - 1], depth + 1, fuel);
                }
                pc++;
                break;
            }
            default:
                pc++;
                break;
        }
    }

    return (sp > 0) ? stack[sp - 1] : 0;
}

/* ============================================================================
 * 5. 8-BYTE TRIANGULAR SUM ANCHORS & TWO-TIERED LEXICASE GATE
 *    4 pairs packed at body[14..21]: (0,0), (5,15), (13,91), (37,191)
 *    Note: T_37 = 37*38/2 = 703 == 191 (mod 256), validating 8-bit wrapping
 *    or exact 16-bit/32-bit T_n = n*(n+1)/2 arithmetic.
 * ============================================================================ */
static void stamp_triangular_anchors(Cell *c) {
    /* (0, 0), (5, 15), (13, 91), (37, 191) */
    const uint8_t anchors[8] = {
        0,   0,
        5,  15,
        13, 91,
        37, 191
    };
    memcpy(c->body + HDR_ANCHORS_OFF, anchors, 8);
}

static int score_anchors(const Cell *c, int64_t *out_total_err) {
    const uint8_t *anc = c->body + HDR_ANCHORS_OFF;
    int passed = 0;
    int64_t err = 0;

    for (int i = 0; i < NUM_ANCHORS; i++) {
        int32_t in_val  = (int32_t)anc[i * 2];
        int32_t exp_val = (int32_t)anc[i * 2 + 1];
        int fuel = MAX_FUEL;
        int32_t got = exec_cell(c, in_val, 0, &fuel);

        /* Compare modulo 256 to match 1-byte anchor encoding cleanly */
        int32_t got_mod = (got & 0xFF);
        int32_t diff = got_mod - exp_val;
        if (diff < 0) diff = -diff;
        if (diff == 0) passed++;
        err += diff;
    }

    if (out_total_err) *out_total_err = err;
    return passed;
}

/* ============================================================================
 * 6. IN-CELL ADAPTIVE PRIORS & STACK-BALANCED MITOSIS SPLICE
 * ============================================================================ */
static void init_priors(Cell *c) {
    for (int i = 0; i < NUM_CODONS; i++) {
        c->body[HDR_WEIGHTS_OFF + i] = 16;
    }
}

static uint8_t sample_codon(const Cell *c, const uint8_t entropy[32], int step) {
    const uint8_t *w = c->body + HDR_WEIGHTS_OFF;
    uint32_t total = 0;
    for (int i = 0; i < NUM_CODONS; i++) total += (w[i] > 0) ? w[i] : 1;

    uint32_t r = ((uint32_t)entropy[step & 31] * 257u + (uint32_t)entropy[(step + 7) & 31]) % total;
    uint32_t acc = 0;
    for (int i = 0; i < NUM_CODONS; i++) {
        acc += (w[i] > 0) ? w[i] : 1;
        if (r < acc) return (uint8_t)i;
    }
    return OP_NOP;
}

static void adapt_prior(Cell *c, uint8_t codon, bool improved) {
    uint8_t idx = codon % NUM_CODONS;
    int w = (int)c->body[HDR_WEIGHTS_OFF + idx];
    w = improved ? (w + 6) : (w - 1);
    if (w < 2) w = 2;
    if (w > 240) w = 240;
    c->body[HDR_WEIGHTS_OFF + idx] = (uint8_t)w;
}

/* Stack-balanced crossover between Parent A and Parent B.
 * Finds seams where both genomes share the exact same stack depth (outside loops),
 * splices prefix(A) + suffix(B), and records a 0xFFFF Mitosis lineage fork cell. */
static bool sacred_splice(const Cell *parent_a, const Cell *parent_b,
                          const uint8_t hash_a[32], const uint8_t hash_b[32],
                          const uint8_t entropy[32], Cell *child_out) {
    uint16_t len_a = get_code_len(parent_a);
    uint16_t len_b = get_code_len(parent_b);
    if (len_a == 0 || len_b == 0 || len_a > MAX_CODE_LEN || len_b > MAX_CODE_LEN) return false;

    const uint8_t *code_a = parent_a->body + CODE_START;
    const uint8_t *code_b = parent_b->body + CODE_START;

    int depth_a[MAX_CODE_LEN + 1], jmp_a[MAX_CODE_LEN];
    int depth_b[MAX_CODE_LEN + 1], jmp_b[MAX_CODE_LEN];
    if (!verify_and_map(code_a, len_a, depth_a, jmp_a)) return false;
    if (!verify_and_map(code_b, len_b, depth_b, jmp_b)) return false;

    /* Collect top-level (loop_depth == 0) splice seams */
    uint16_t valid_a[MAX_CODE_LEN], valid_b[MAX_CODE_LEN];
    int count = 0;

    int ld_a = 0;
    for (uint16_t i = 0; i < len_a && count < MAX_CODE_LEN; i++) {
        uint8_t op_a = code_a[i] % NUM_CODONS;
        if (op_a == OP_LOOP) ld_a++;
        if (ld_a == 0 && i > 0) {
            int target_sp = depth_a[i];
            int ld_b = 0;
            for (uint16_t j = 1; j < len_b; j++) {
                uint8_t op_b = code_b[j] % NUM_CODONS;
                if (op_b == OP_LOOP) ld_b++;
                if (ld_b == 0 && depth_b[j] == target_sp) {
                    uint16_t new_len = i + (len_b - j);
                    if (new_len <= MAX_CODE_LEN) {
                        valid_a[count] = i;
                        valid_b[count] = j;
                        count++;
                        break;
                    }
                }
                if (op_b == OP_END) ld_b--;
            }
        }
        if (op_a == OP_END) ld_a--;
    }

    if (count == 0) return false;

    int pick = entropy[0] % count;
    uint16_t cut_a = valid_a[pick];
    uint16_t cut_b = valid_b[pick];
    uint16_t child_len = cut_a + (len_b - cut_b);

    /* Create & store 0xFFFF Mitosis fork record linking hash_a and hash_b */
    Cell fork_cell;
    memset(&fork_cell, 0, sizeof(Cell));
    memcpy(fork_cell.left, hash_a, 32);
    memcpy(fork_cell.right, hash_b, 32);
    set_code_len(&fork_cell, FORK_MARKER);
    uint8_t fork_hash[32];
    arena_put(&fork_cell, fork_hash);

    /* Assemble child Cell with blended adaptive priors */
    memset(child_out, 0, sizeof(Cell));
    memcpy(child_out->left, hash_a, 32);
    memcpy(child_out->right, fork_hash, 32);
    set_code_len(child_out, child_len);

    for (int k = 0; k < NUM_CODONS; k++) {
        uint16_t blend = (uint16_t)parent_a->body[HDR_WEIGHTS_OFF + k] +
                         (uint16_t)parent_b->body[HDR_WEIGHTS_OFF + k];
        child_out->body[HDR_WEIGHTS_OFF + k] = (uint8_t)(blend >> 1);
    }
    stamp_triangular_anchors(child_out);

    memcpy(child_out->body + CODE_START, code_a, cut_a);
    memcpy(child_out->body + CODE_START + cut_a, code_b + cut_b, len_b - cut_b);

    int check_d[MAX_CODE_LEN + 1], check_j[MAX_CODE_LEN];
    return verify_and_map(child_out->body + CODE_START, child_len, check_d, check_j);
}

/* ============================================================================
 * 7. MUTATION & MAIN EVOLUTIONARY LOOP
 * ============================================================================ */
static bool mutate_cell(const Cell *parent, const uint8_t parent_hash[32],
                        const uint8_t entropy[32], Cell *child_out, uint8_t *mutated_codon) {
    *child_out = *parent;
    memcpy(child_out->left, parent_hash, 32);

    uint16_t len = get_code_len(child_out);
    if (len == 0 || len > MAX_CODE_LEN) return false;

    uint8_t *code = child_out->body + CODE_START;
    uint8_t action = entropy[1] % 3;
    uint8_t codon = sample_codon(child_out, entropy, 2);
    *mutated_codon = codon;

    if (action == 0 && len < MAX_CODE_LEN) {
        /* Insert codon */
        uint16_t pos = entropy[3] % (len + 1);
        memmove(code + pos + 1, code + pos, len - pos);
        code[pos] = codon;
        len++;
    } else if (action == 1 && len > 2) {
        /* Delete codon */
        uint16_t pos = entropy[3] % len;
        memmove(code + pos, code + pos + 1, len - pos - 1);
        len--;
    } else {
        /* Point-mutate codon */
        uint16_t pos = entropy[3] % len;
        code[pos] = codon;
    }

    set_code_len(child_out, len);
    int dmap[MAX_CODE_LEN + 1], jmap[MAX_CODE_LEN];
    return verify_and_map(code, len, dmap, jmap);
}

static void build_seed_cell(Cell *c) {
    memset(c, 0, sizeof(Cell));
    init_priors(c);
    stamp_triangular_anchors(c);

    /* Initial seed genome: computes x (passes anchor (0,0) = 1/4 immediately) */
    const uint8_t seed_code[] = {
        OP_IN, OP_DUP, OP_ONE, OP_ADD, OP_MUL, OP_SHR
    };
    /* Note: seed_code actually computes T_x = x*(x+1)/2 directly!
     * Let's start with a near-miss seed so evolution actively discovers & refines:
     * x * x >> 1 (passes (0,0) and (5,12 vs 15)) */
    const uint8_t imperfect_seed[] = {
        OP_IN, OP_DUP, OP_MUL, OP_SHR
    };
    (void)seed_code;
    set_code_len(c, sizeof(imperfect_seed));
    memcpy(c->body + CODE_START, imperfect_seed, sizeof(imperfect_seed));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <local_udp_port> [peer_udp_port]\n", argv[0]);
        return 1;
    }

    int local_port = atoi(argv[1]);
    int peer_port  = (argc >= 3) ? atoi(argv[2]) : 0;

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket");
        return 1;
    }
    int flags = fcntl(udp_sock, F_GETFL, 0);
    fcntl(udp_sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    laddr.sin_port = htons((uint16_t)local_port);

    if (bind(udp_sock, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        perror("bind");
        close(udp_sock);
        return 1;
    }

    if (peer_port > 0) {
        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer_addr.sin_port = htons((uint16_t)peer_port);
        has_peer = true;
    }

    /* Bootstrap genesis champion & breeding partner (ancestor_b) */
    Cell champ, ancestor_b;
    build_seed_cell(&champ);
    uint8_t champ_hash[32], ancestor_b_hash[32];
    arena_put(&champ, champ_hash);

    ancestor_b = champ;
    memcpy(ancestor_b_hash, champ_hash, 32);

    int64_t champ_err = 0;
    int champ_anchors = score_anchors(&champ, &champ_err);

    printf("[genesis] Node online 127.0.0.1:%d (peer=%d) | seed anchors=%d/4 err=%lld\n",
           local_port, peer_port, champ_anchors, (long long)champ_err);

    uint8_t entropy[32];
    uint64_t seed_state[2] = { (uint64_t)time(NULL), (uint64_t)getpid() };
    arx_256(seed_state, sizeof(seed_state), entropy);

    for (uint64_t gen = 1; ; gen++) {
        /* 1. Poll UDP socket for cache-miss requests & migrant cells */
        uint8_t migrant_hash[32];
        bool got_migrant = false;
        poll_network(migrant_hash, &got_migrant);

        if (got_migrant) {
            int mslot = arena_lookup(migrant_hash);
            if (mslot >= 0 && get_code_len(&arena[mslot]) != FORK_MARKER) {
                int64_t merr = 0;
                int manc = score_anchors(&arena[mslot], &merr);
                if (manc >= 3 && (manc > champ_anchors || (manc == champ_anchors && merr < champ_err))) {
                    champ = arena[mslot];
                    memcpy(champ_hash, migrant_hash, 32);
                    champ_anchors = manc;
                    champ_err = merr;
                } else if (manc >= 3) {
                    ancestor_b = arena[mslot];
                    memcpy(ancestor_b_hash, migrant_hash, 32);
                }
            }
        }

        /* 2. Advance deterministic ARX entropy stream */
        uint8_t mix_buf[64];
        memcpy(mix_buf, entropy, 32);
        memcpy(mix_buf + 32, champ_hash, 32);
        arx_256(mix_buf, sizeof(mix_buf), entropy);

        /* 3. Generate candidate via Mitosis crossover (25%) or Adaptive Mutation (75%) */
        Cell candidate;
        uint8_t mutated_codon = OP_NOP;
        bool valid = false;
        bool did_mutate = false;

        if ((entropy[0] & 3) == 0) {
            valid = sacred_splice(&champ, &ancestor_b, champ_hash, ancestor_b_hash, entropy, &candidate);
        }
        if (!valid) {
            valid = mutate_cell(&champ, champ_hash, entropy, &candidate, &mutated_codon);
            did_mutate = true;
        }

        if (valid) {
            int64_t cand_err = 0;
            int cand_anchors = score_anchors(&candidate, &cand_err);
            bool strictly_better = (cand_anchors > champ_anchors) ||
                                   (cand_anchors == champ_anchors && cand_err < champ_err);

            if (did_mutate) {
                adapt_prior(&champ, mutated_codon, strictly_better);
            }

            /* Two-tiered lexicase gate:
             *   Tier 1: >= 3/4 anchors qualifies candidate to enter breeding pool (ancestor_b)
             *   Tier 2: 4/4 anchors (or strictly better anchor score) promotes to champ */
            if (cand_anchors >= 3) {
                uint8_t cand_hash[32];
                arena_put(&candidate, cand_hash);

                if (strictly_better && (cand_anchors == 4 || cand_anchors > champ_anchors)) {
                    ancestor_b = champ;
                    memcpy(ancestor_b_hash, champ_hash, 32);

                    champ = candidate;
                    memcpy(champ_hash, cand_hash, 32);
                    champ_anchors = cand_anchors;
                    champ_err = cand_err;

                    /* Broadcast newly crowned champion to peer */
                    if (has_peer) {
                        sendto(udp_sock, &champ, CELL_BYTES, 0,
                               (struct sockaddr *)&peer_addr, sizeof(peer_addr));
                    }
                } else {
                    ancestor_b = candidate;
                    memcpy(ancestor_b_hash, cand_hash, 32);
                }
            }
        }

        /* 4. Periodic telemetry */
        if ((gen % 10000) == 0) {
            int f10 = MAX_FUEL;
            int32_t t10 = exec_cell(&champ, 10, 0, &f10);
            printf("[gen=%llu] anchors=%d/4 err=%lld len=%u T(10)=%d (expected 55)\n",
                   (unsigned long long)gen,
                   champ_anchors,
                   (long long)champ_err,
                   get_code_len(&champ),
                   t10);
            fflush(stdout);
        }
    }

    close(udp_sock);
    return 0;
}
