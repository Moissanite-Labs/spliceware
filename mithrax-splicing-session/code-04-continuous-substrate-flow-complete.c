#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// ─── 1. THE UNIVERSAL ATOM (RAM == DISK == WIRE == HASH PRE-IMAGE) ───────────
typedef struct __attribute__((packed)) {
    uint8_t left[32];    // Operator / Causal Parent / Synth Cell Hash
    uint8_t right[32];   // Operand  / Input Payload / Fault Cell Hash
    uint8_t body[1024];  // [0..1]: Fuel | [2..N]: Raw Opcodes or Data Bytes
} Cell;                  // Exact 1,088 bytes: fits cleanly inside 1 UDP MTU

#define ARENA_BITS 16
#define ARENA_CAP  (1 << ARENA_BITS)

static Cell    arena[ARENA_CAP];
static uint8_t names[ARENA_CAP][32];
static uint8_t occupied[ARENA_CAP];
static uint8_t synth_hash[32];

// ─── 2. 256-BIT ARX SPONGE & O(1) CONTENT-ADDRESSED ARENA ────────────────────
static void hash_cell(const Cell *c, uint8_t out[32]) {
    uint64_t s[4] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                     0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL};
    const uint64_t *w = (const uint64_t *)c;
    for (int i = 0; i < (int)(sizeof(Cell) / 8); i++) {
        s[i & 3] ^= w[i];
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

// Open-addressed O(1) Arena lookup & idempotent insertion
static int resolve(const uint8_t h[32]) {
    uint32_t idx = *(const uint32_t *)h & (ARENA_CAP - 1);
    for (int n = 0; n < ARENA_CAP; n++) {
        uint32_t slot = (idx + n) & (ARENA_CAP - 1);
        if (!occupied[slot]) return -1; // Cache miss!
        if (memcmp(names[slot], h, 32) == 0) return (int)slot;
    }
    return -1;
}

static int ingest(const Cell *c, uint8_t out_h[32]) {
    uint8_t h[32];
    hash_cell(c, h);
    if (out_h) memcpy(out_h, h, 32);
    uint32_t idx = *(const uint32_t *)h & (ARENA_CAP - 1);
    for (int n = 0; n < ARENA_CAP; n++) {
        uint32_t slot = (idx + n) & (ARENA_CAP - 1);
        if (!occupied[slot] || memcmp(names[slot], h, 32) == 0) {
            arena[slot] = *c;
            memcpy(names[slot], h, 32);
            occupied[slot] = 1;
            return (int)slot;
        }
    }
    return -1;
}

// ─── 3. THE CONTINUOUS FLOW (UDP WIRE <-> GRAPH REDUCTION <-> SYNTHESIS) ─────
void flow(int sock, struct sockaddr_in *peer, uint8_t root[32]) {
    uint8_t pkt[sizeof(Cell)];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);

    // A. INGEST OR SERVE OVER RAW UDP (Zero parsing, zero headers)
    ssize_t n = recvfrom(sock, pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&src, &slen);
    if (n == 32) {
        // 32B packet == A peer experienced a cache miss; spill the Cell back if we have it
        int hit = resolve(pkt);
        if (hit >= 0) sendto(sock, &arena[hit], sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)&src, slen);
    } else if (n == sizeof(Cell)) {
        // 1,088B packet == Raw Cell arrived; hash-verify & drop directly into Arena
        uint8_t arrived[32];
        ingest((const Cell *)pkt, arrived);
        // Symmetric causal convergence: lexicographically higher hash wins ties deterministically
        if (memcmp(arrived, root, 32) > 0) memcpy(root, arrived, 32);
    }

    // B. DEREFERENCE ACTIVE ROOT (Cache Miss == 32B UDP Wire Spill)
    int r_slot = resolve(root);
    if (r_slot < 0) { sendto(sock, root, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
    Cell *active = &arena[r_slot];
    if (is_zero(active->left)) return; // Pure leaf value; already fully reduced

    int f_slot = resolve(active->left);
    if (f_slot < 0) { sendto(sock, active->left, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }
    int x_slot = resolve(active->right);
    if (x_slot < 0) { sendto(sock, active->right, 32, MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer)); return; }

    // C. METERED IN-PLACE STACK REDUCTION (Zero ambient authority)
    uint16_t fuel = *(uint16_t *)active->body;
    const uint8_t *code = arena[f_slot].body;
    const uint8_t *in   = arena[x_slot].body;

    Cell out = {0};
    uint8_t stack[64] = {0};
    int sp = 0, pc = 0, wp = 2, trapped = 0;

    while (pc < 1024 && code[pc] != 0x00) { // 0x00 == HALT (Valid Reduction)
        if (fuel-- == 0) { trapped = 1; break; } // OutOfFuel Trap!
        uint8_t op = code[pc++];
        if      (op == 0x01 && sp < 64 && pc < 1024) stack[sp++] = code[pc++];              // PUSH imm
        else if (op == 0x02 && sp > 0)               stack[sp-1] = in[stack[sp-1] & 1023];  // LOAD input[idx]
        else if (op == 0x03 && sp > 1)             { stack[sp-2] += stack[sp-1]; sp--; }    // ADD
        else if (op == 0x04 && sp > 1)             { stack[sp-2] ^= stack[sp-1]; sp--; }    // XOR
        else if (op == 0x05 && sp > 0 && wp < 1024)  out.body[wp++] = stack[--sp];          // EMIT byte
        else if (op == 0x06 && sp > 0)             { if (!stack[--sp]) { trapped = 2; break; } } // ASSERT invariant
        else { trapped = 3; break; } // Invalid Opcode / Stack Bounds Trap!
    }

    // D. TRAP-TO-SYNTHESIS OR CAUSAL ADVANCE
    if (trapped) {
        // Turn the fault itself into a Cell: [Op | Input | TrapCode + PC]
        Cell fault = {0};
        memcpy(fault.left,  active->left,  32);
        memcpy(fault.right, active->right, 32);
        fault.body[2] = (uint8_t)trapped;
        fault.body[3] = (uint8_t)(pc & 0xFF);
        uint8_t fault_h[32];
        ingest(&fault, fault_h);

        // Redirect root straight into [Synthesizer | FaultCell | FreshFuel]
        Cell mutate = {0};
        memcpy(mutate.left,  synth_hash, 32);
        memcpy(mutate.right, fault_h,    32);
        *(uint16_t *)mutate.body = 512;
        ingest(&mutate, root);
    } else {
        // Link causal parent (root) into reduced output & broadcast into the mesh
        memcpy(out.right, root, 32); // Causal lineage preserved in right link
        ingest(&out, root);
        sendto(sock, &out, sizeof(Cell), MSG_DONTWAIT, (struct sockaddr *)peer, sizeof(*peer));
    }
}
