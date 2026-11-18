#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* ============================================================================
 * 1. THE 1,088-BYTE ATOMIC CELL & HEADER GEOMETRY
 * ============================================================================ */
#define CELL_BYTES   1088
#define HDR_BYTES    64
#define PAYLOAD_SIZE 1024
#define POP_SIZE     64
#define MAX_STEPS    256
#define MAX_CALL_D   16

/* Header offsets (inside 64-byte header, indices 0..15 as int32_t) */
#define HDR_GENERATION 0   /* 4B: Generation counter */
#define HDR_FITNESS    1   /* 4B: Last evaluated fitness score */
#define HDR_WEIGHTS    2   /* 32B (8 x int32_t, indices 2..9): Adaptive prior weights */
#define HDR_ANCHOR_LO  14  /* 4B: Low word of 8-byte Triangular Sum Anchor */
#define HDR_ANCHOR_HI  15  /* 4B: High word of 8-byte Triangular Sum Anchor */

typedef struct __attribute__((packed)) {
    uint8_t header[HDR_BYTES];
    uint8_t payload[PAYLOAD_SIZE];
} Cell;

_Static_assert(sizeof(Cell) == CELL_BYTES, "Cell must be strictly 1088 bytes");

/* ============================================================================
 * 2. INSTRUCTION SET (24 DETERMINISTIC CODONS + NO-OP MODULO SPACE)
 * ============================================================================ */
enum {
    OP_NOP   = 0x00,
    OP_CONST = 0x01,
    OP_ADD   = 0x02,
    OP_SUB   = 0x03,
    OP_MUL   = 0x04,
    OP_DIV   = 0x05,
    OP_MOD   = 0x06,
    OP_XOR   = 0x07,
    OP_AND   = 0x08,
    OP_OR    = 0x09,
    OP_SHL   = 0x0A,
    OP_SHR   = 0x0B,
    OP_LOAD  = 0x0C,
    OP_STORE = 0x0D,
    OP_JMP   = 0x0E,
    OP_JZ    = 0x0F,
    OP_JNZ   = 0x10,
    OP_CMP   = 0x11,
    OP_DUP   = 0x12, /* Duplicate Top of Stack: [..., a] -> [..., a, a] */
    OP_OVER  = 0x13, /* Copy Second Element:    [..., a, b] -> [..., a, b, a] */
    OP_SWAP  = 0x14,
    OP_DROP  = 0x15,
    OP_LOOP  = 0x16, /* Bounded relative loop: pops count, repeats next block */
    OP_CALL  = 0x17, /* Bounded subroutine call: pushes return PC, jumps to target */
    OP_RET   = 0x18, /* Return from CALL frame */
    OP_HALT  = 0xFF
};

/* ============================================================================
 * 3. THE 8-BYTE TRIANGULAR SUM ANCHOR GATE
 *    Checks that a 64-bit integer N is a triangular number: 8*N + 1 == k^2
 * ============================================================================ */
static inline uint64_t isqrt_u64(uint64_t n) {
    if (n < 2) return n;
    uint64_t x = 1ULL << ((64 - __builtin_clzll(n)) >> 1);
    while (1) {
        uint64_t y = (x + n / x) >> 1;
        if (y >= x) return x;
        x = y;
    }
}

static int verify_triangular_anchor(const Cell *c) {
    const int32_t *hdr = (const int32_t *)c->header;
    uint64_t lo = (uint32_t)hdr[HDR_ANCHOR_LO];
    uint64_t hi = (uint32_t)hdr[HDR_ANCHOR_HI];
    uint64_t anchor = (hi << 32) | lo;

    if (anchor == 0) return 0;

    /* Guard against overflow in 8 * anchor + 1 */
    if (anchor > (UINT64_MAX - 1ULL) / 8ULL) return 0;

    uint64_t disc = 8ULL * anchor + 1ULL;
    uint64_t root = isqrt_u64(disc);
    return (root * root == disc) ? 1 : 0;
}

static void stamp_triangular_anchor(Cell *c, uint64_t k) {
    /* T_k = k * (k + 1) / 2 */
    uint64_t anchor = (k & 1ULL) ? k * ((k + 1ULL) >> 1) : (k >> 1) * (k + 1ULL);
    int32_t *hdr = (int32_t *)c->header;
    hdr[HDR_ANCHOR_LO] = (int32_t)(anchor & 0xFFFFFFFFULL);
    hdr[HDR_ANCHOR_HI] = (int32_t)((anchor >> 32) & 0xFFFFFFFFULL);
}

/* ============================================================================
 * 4. DETERMINISTIC SANDBOX VM (WITH LOOP, CALL, DUP, OVER)
 * ============================================================================ */
typedef struct {
    int32_t stack[128];
    int sp;
    int32_t mem[64];
    int call_stack[MAX_CALL_D];
    int csp;
    int loop_pc[MAX_CALL_D];
    int loop_cnt[MAX_CALL_D];
    int lsp;
    int steps;
} VM;

static inline void vm_push(VM *vm, int32_t v) {
    if (vm->sp < 128) vm->stack[vm->sp++] = v;
}

static inline int32_t vm_pop(VM *vm) {
    return (vm->sp > 0) ? vm->stack[--vm->sp] : 0;
}

static int32_t run_cell(const Cell *c, int32_t input_x) {
    VM vm;
    memset(&vm, 0, sizeof(VM));
    vm.mem[0] = input_x;

    int pc = 0;
    while (pc >= 0 && pc < PAYLOAD_SIZE && vm.steps < MAX_STEPS) {
        uint8_t op = c->payload[pc++];
        vm.steps++;

        switch (op) {
            case OP_NOP:
                break;

            case OP_CONST: {
                if (pc >= PAYLOAD_SIZE) goto done;
                int8_t imm = (int8_t)c->payload[pc++];
                vm_push(&vm, (int32_t)imm);
                break;
            }

            case OP_ADD: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (int32_t)((uint32_t)a + (uint32_t)b));
                break;
            }

            case OP_SUB: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (int32_t)((uint32_t)a - (uint32_t)b));
                break;
            }

            case OP_MUL: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (int32_t)((uint32_t)a * (uint32_t)b));
                break;
            }

            case OP_DIV: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (b != 0 && !(a == INT32_MIN && b == -1)) ? (a / b) : 0);
                break;
            }

            case OP_MOD: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (b != 0 && !(a == INT32_MIN && b == -1)) ? (a % b) : 0);
                break;
            }

            case OP_XOR: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, a ^ b);
                break;
            }

            case OP_AND: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, a & b);
                break;
            }

            case OP_OR: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, a | b);
                break;
            }

            case OP_SHL: {
                int32_t b = vm_pop(&vm) & 31, a = vm_pop(&vm);
                vm_push(&vm, (int32_t)((uint32_t)a << b));
                break;
            }

            case OP_SHR: {
                int32_t b = vm_pop(&vm) & 31, a = vm_pop(&vm);
                vm_push(&vm, (int32_t)((uint32_t)a >> b));
                break;
            }

            case OP_LOAD: {
                int32_t idx = vm_pop(&vm) & 63;
                vm_push(&vm, vm.mem[idx]);
                break;
            }

            case OP_STORE: {
                int32_t idx = vm_pop(&vm) & 63;
                int32_t val = vm_pop(&vm);
                vm.mem[idx] = val;
                break;
            }

            case OP_JMP: {
                if (pc >= PAYLOAD_SIZE) goto done;
                int8_t off = (int8_t)c->payload[pc++];
                int target = pc + off;
                if (target >= 0 && target < PAYLOAD_SIZE) pc = target;
                break;
            }

            case OP_JZ: {
                if (pc >= PAYLOAD_SIZE) goto done;
                int8_t off = (int8_t)c->payload[pc++];
                if (vm_pop(&vm) == 0) {
                    int target = pc + off;
                    if (target >= 0 && target < PAYLOAD_SIZE) pc = target;
                }
                break;
            }

            case OP_JNZ: {
                if (pc >= PAYLOAD_SIZE) goto done;
                int8_t off = (int8_t)c->payload[pc++];
                if (vm_pop(&vm) != 0) {
                    int target = pc + off;
                    if (target >= 0 && target < PAYLOAD_SIZE) pc = target;
                }
                break;
            }

            case OP_CMP: {
                int32_t b = vm_pop(&vm), a = vm_pop(&vm);
                vm_push(&vm, (a < b) ? -1 : (a > b ? 1 : 0));
                break;
            }

            /* Stack manipulation codons: DUP and OVER */
            case OP_DUP: {
                if (vm.sp > 0) vm_push(&vm, vm.stack[vm.sp - 1]);
                break;
            }

            case OP_OVER: {
                if (vm.sp > 1) vm_push(&vm, vm.stack[vm.sp - 2]);
                break;
            }

            case OP_SWAP: {
                if (vm.sp > 1) {
                    int32_t t = vm.stack[vm.sp - 1];
                    vm.stack[vm.sp - 1] = vm.stack[vm.sp - 2];
                    vm.stack[vm.sp - 2] = t;
                }
                break;
            }

            case OP_DROP: {
                (void)vm_pop(&vm);
                break;
            }

            /* Deterministic bounded LOOP:
             * Operand 1: loop body length in bytes (1..64)
             * Stack top: iteration count (clamped to 0..32) */
            case OP_LOOP: {
                if (pc >= PAYLOAD_SIZE) goto done;
                uint8_t body_len = (c->payload[pc++] & 63) + 1;
                int32_t count = vm_pop(&vm);
                if (count < 0) count = 0;
                if (count > 32) count = 32;

                if (count > 1 && vm.lsp < MAX_CALL_D) {
                    vm.loop_pc[vm.lsp] = pc;
                    vm.loop_cnt[vm.lsp] = count - 1;
                    vm.lsp++;
                } else if (count == 0) {
                    int skip = pc + body_len;
                    pc = (skip < PAYLOAD_SIZE) ? skip : PAYLOAD_SIZE;
                }
                break;
            }

            /* Deterministic bounded CALL & RET */
            case OP_CALL: {
                if (pc >= PAYLOAD_SIZE) goto done;
                uint8_t target_page = c->payload[pc++];
                int target = (target_page * 4) % PAYLOAD_SIZE;
                if (vm.csp < MAX_CALL_D) {
                    vm.call_stack[vm.csp++] = pc;
                    pc = target;
                }
                break;
            }

            case OP_RET: {
                if (vm.lsp > 0) {
                    int top = vm.lsp - 1;
                    if (vm.loop_cnt[top] > 0) {
                        vm.loop_cnt[top]--;
                        pc = vm.loop_pc[top];
                        break;
                    } else {
                        vm.lsp--;
                    }
                }
                if (vm.csp > 0) {
                    pc = vm.call_stack[--vm.csp];
                } else {
                    goto done;
                }
                break;
            }

            case OP_HALT:
                goto done;

            default:
                /* Unmapped byte values act as evolutionary neutral drift (NOP) */
                break;
        }
    }

done:
    return (vm.sp > 0) ? vm.stack[vm.sp - 1] : vm.mem[1];
}

/* ============================================================================
 * 5. FITNESS & ADAPTIVE PRIORS (SELF-TUNING MUTATION BIAS)
 * ============================================================================ */
static int32_t target_fn(int32_t x) {
    /* Evolutionary target: f(x) = x^2 + 3x + 7 */
    return (x * x) + (3 * x) + 7;
}

static int32_t evaluate_cell(Cell *c) {
    /* Enforce the 8-byte Triangular Sum Anchor Gate before execution */
    if (!verify_triangular_anchor(c)) {
        int32_t *hdr = (int32_t *)c->header;
        hdr[HDR_FITNESS] = INT32_MIN;
        return INT32_MIN;
    }

    int64_t total_err = 0;
    const int32_t tests[] = { -5, -2, -1, 0, 1, 2, 4, 7 };
    for (int i = 0; i < 8; i++) {
        int32_t out = run_cell(c, tests[i]);
        int64_t diff = (int64_t)out - (int64_t)target_fn(tests[i]);
        if (diff < 0) diff = -diff;
        if (diff > 1000000) diff = 1000000;
        total_err += diff;
    }

    int32_t fitness = (int32_t)(10000000LL - total_err);
    int32_t *hdr = (int32_t *)c->header;
    hdr[HDR_FITNESS] = fitness;
    return fitness;
}

/* Updates the cell's 8-element adaptive prior vector stored in its 64B header
 * based on whether the last mutation improved fitness. */
static void adapt_prior(Cell *c, int32_t prev_fitness, int32_t new_fitness, uint8_t mutated_opcode) {
    int32_t *hdr = (int32_t *)c->header;
    int bucket = mutated_opcode & 7;
    int32_t delta = (new_fitness > prev_fitness) ? 12 : -4;

    int32_t w = hdr[HDR_WEIGHTS + bucket] + delta;
    if (w < 10) w = 10;
    if (w > 1000) w = 1000;
    hdr[HDR_WEIGHTS + bucket] = w;
}

static uint8_t sample_weighted_opcode(const Cell *c) {
    const int32_t *hdr = (const int32_t *)c->header;
    int32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        int32_t w = hdr[HDR_WEIGHTS + i];
        sum += (w > 0) ? w : 10;
    }
    int32_t r = rand() % sum;
    int32_t acc = 0;
    int chosen_bucket = 0;
    for (int i = 0; i < 8; i++) {
        int32_t w = hdr[HDR_WEIGHTS + i];
        acc += (w > 0) ? w : 10;
        if (r < acc) {
            chosen_bucket = i;
            break;
        }
    }
    /* Map bucket (0..7) to active opcode range (0x00..0x18) */
    uint8_t base = (uint8_t)((chosen_bucket * 3) + (rand() % 3));
    return (base <= OP_RET) ? base : OP_NOP;
}

static void mutate_cell(Cell *c) {
    int32_t *hdr = (int32_t *)c->header;
    int32_t prev_fit = hdr[HDR_FITNESS];

    int mutations = 1 + (rand() % 3);
    uint8_t last_op = OP_NOP;
    for (int m = 0; m < mutations; m++) {
        int pos = rand() % PAYLOAD_SIZE;
        last_op = sample_weighted_opcode(c);
        c->payload[pos] = last_op;
    }

    /* Small chance to evolve the Triangular Anchor to next T_{k+1} */
    if ((rand() % 16) == 0) {
        uint64_t k = 1 + (rand() % 1024);
        stamp_triangular_anchor(c, k);
    }

    hdr[HDR_GENERATION]++;
    int32_t new_fit = evaluate_cell(c);
    adapt_prior(c, prev_fit, new_fit, last_op);
}

/* ============================================================================
 * 6. UDP PEER-TO-PEER HORIZONTAL GENE TRANSFER & MAIN EVOLUTION LOOP
 * ============================================================================ */
static void seed_cell(Cell *c, uint64_t anchor_k) {
    memset(c, 0, sizeof(Cell));
    int32_t *hdr = (int32_t *)c->header;
    for (int i = 0; i < 8; i++) hdr[HDR_WEIGHTS + i] = 100;
    stamp_triangular_anchor(c, anchor_k);

    /* Seed initial genome with DUP / OVER / arithmetic building blocks */
    uint8_t bootstrap[] = {
        OP_CONST, 0, OP_LOAD,  /* push x */
        OP_DUP,                /* [x, x] */
        OP_MUL,                /* [x^2] */
        OP_CONST, 0, OP_LOAD,  /* [x^2, x] */
        OP_CONST, 3, OP_MUL,   /* [x^2, 3x] */
        OP_ADD,                /* [x^2 + 3x] */
        OP_CONST, 1, OP_ADD,   /* initial slightly off constant (+1 vs +7) */
        OP_HALT
    };
    memcpy(c->payload, bootstrap, sizeof(bootstrap));
    evaluate_cell(c);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <local_udp_port> [peer_udp_port]\n", argv[0]);
        return 1;
    }

    srand((unsigned int)(time(NULL) ^ getpid()));

    int local_port = atoi(argv[1]);
    int peer_port  = (argc >= 3) ? atoi(argv[2]) : 0;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local_addr.sin_port = htons((uint16_t)local_port);

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    struct sockaddr_in peer_addr;
    memset(&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (peer_port > 0) peer_addr.sin_port = htons((uint16_t)peer_port);

    Cell arena[POP_SIZE];
    for (int i = 0; i < POP_SIZE; i++) {
        seed_cell(&arena[i], (uint64_t)(i + 1));
    }

    printf("[genesis] Node online on 127.0.0.1:%d (Cell=%zuB, Peer=%d)\n",
           local_port, sizeof(Cell), peer_port);

    for (uint64_t tick = 0; ; tick++) {
        /* 1. Ingest incoming 1,088-byte UDP migrant cells gated by Triangular Anchor */
        Cell migrant;
        ssize_t n = recvfrom(sock, &migrant, sizeof(Cell), 0, NULL, NULL);
        if (n == sizeof(Cell) && verify_triangular_anchor(&migrant)) {
            int worst_idx = 0;
            int32_t worst_fit = ((int32_t *)arena[0].header)[HDR_FITNESS];
            for (int i = 1; i < POP_SIZE; i++) {
                int32_t f = ((int32_t *)arena[i].header)[HDR_FITNESS];
                if (f < worst_fit) {
                    worst_fit = f;
                    worst_idx = i;
                }
            }
            if (evaluate_cell(&migrant) > worst_fit) {
                arena[worst_idx] = migrant;
            }
        }

        /* 2. Tournament selection + adaptive mutation */
        int a = rand() % POP_SIZE;
        int b = rand() % POP_SIZE;
        int32_t fit_a = ((int32_t *)arena[a].header)[HDR_FITNESS];
        int32_t fit_b = ((int32_t *)arena[b].header)[HDR_FITNESS];

        int winner = (fit_a >= fit_b) ? a : b;
        int loser  = (fit_a >= fit_b) ? b : a;

        arena[loser] = arena[winner];
        mutate_cell(&arena[loser]);

        /* 3. Periodic telemetry & horizontal gene transfer to peer */
        if ((tick % 5000) == 0) {
            int best_idx = 0;
            int32_t best_fit = ((int32_t *)arena[0].header)[HDR_FITNESS];
            for (int i = 1; i < POP_SIZE; i++) {
                int32_t f = ((int32_t *)arena[i].header)[HDR_FITNESS];
                if (f > best_fit) {
                    best_fit = f;
                    best_idx = i;
                }
            }

            const int32_t *bhdr = (const int32_t *)arena[best_idx].header;
            uint64_t anchor = ((uint64_t)(uint32_t)bhdr[HDR_ANCHOR_HI] << 32) |
                              (uint32_t)bhdr[HDR_ANCHOR_LO];

            printf("[tick=%llu] best_fit=%d gen=%d anchor=%llu f(4)=%d (target=%d)\n",
                   (unsigned long long)tick,
                   best_fit,
                   bhdr[HDR_GENERATION],
                   (unsigned long long)anchor,
                   run_cell(&arena[best_idx], 4),
                   target_fn(4));
            fflush(stdout);

            if (peer_port > 0) {
                sendto(sock, &arena[best_idx], sizeof(Cell), 0,
                       (struct sockaddr *)&peer_addr, sizeof(peer_addr));
            }
        }
    }

    close(sock);
    return 0;
}