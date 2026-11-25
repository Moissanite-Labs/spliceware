/* test_c11_hardening.c — ASan + UBSan regression suite for C11 substrates (S1..S4)
 *
 * Build & run via `make -C substrates/c11 test` (compiles each substrate under
 * `-std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all`
 * in a temporary directory cleaned up via shell trap; uses GCC/Clang
 * `__attribute__((packed))` 1-byte alignment on `Cell`).
 */
#define SPLICEWARE_TEST_HARNESS 1

#if defined(TEST_SUBSTRATE_V1)
#include "v1_genesis.c"

static int check_cond(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "[FAIL v1] %s\n", msg);
        return 1;
    }
    return 0;
}

static int bind_loopback_udp(uint16_t *out_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(s);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, (struct sockaddr *)&addr, &alen) < 0) {
        close(s);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);
    return s;
}

static void reset_arena(void) {
    memset(arena, 0, sizeof(arena));
    memset(names, 0, sizeof(names));
    memset(occupied, 0, sizeof(occupied));
    memset(ancestor_a, 0, sizeof(ancestor_a));
    memset(ancestor_b, 0, sizeof(ancestor_b));
}

int main(void) {
    int fails = 0;

    /* S3: Unaligned odd-offset Cell hashing, resolve, and UDP datagram ingest under UBSan */
    reset_arena();
    Cell aligned;
    memset(&aligned, 0, sizeof(aligned));
    set_body_u16(&aligned, 512);
    aligned.body[2] = 0x01;
    aligned.body[3] = 0x42;
    aligned.body[4] = 0x05;
    aligned.body[5] = 0x00;

    uint8_t odd_buf[sizeof(Cell) + 1];
    odd_buf[0] = 0xEE;
    memcpy(odd_buf + 1, &aligned, sizeof(Cell));

    uint8_t h_aligned[32], h_odd[32];
    hash_cell(&aligned, h_aligned);
    hash_cell((const Cell *)(odd_buf + 1), h_odd);
    fails += check_cond(memcmp(h_aligned, h_odd, 32) == 0, "S3: odd-offset hash_cell parity");

    int slot = ingest(&aligned, NULL);
    fails += check_cond(slot >= 0, "S3: aligned ingest succeeds");

    uint8_t odd_hash_buf[33];
    odd_hash_buf[0] = 0xCC;
    memcpy(odd_hash_buf + 1, h_aligned, 32);
    fails += check_cond(resolve(odd_hash_buf + 1) == slot, "S3: odd-offset resolve succeeds");

    /* S4: flow() with unresident/zero champ and unresident ancestor_b must not index arena[-1] */
    reset_arena();
    uint16_t node_port = 0, peer_port = 0;
    int node_sock = bind_loopback_udp(&node_port);
    int peer_sock = bind_loopback_udp(&peer_port);
    if (check_cond(node_sock >= 0 && peer_sock >= 0, "S4: loopback UDP sockets created") != 0) {
        if (node_sock >= 0) close(node_sock);
        if (peer_sock >= 0) close(peer_sock);
        return 1;
    }
    struct sockaddr_in peer_sa;
    memset(&peer_sa, 0, sizeof(peer_sa));
    peer_sa.sin_family = AF_INET;
    peer_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer_sa.sin_port = htons(peer_port);

    /* Build a trapping function cell (invalid opcode 0x7F -> step_branch returns 0 -> trap path) */
    Cell trap_fn = {0};
    set_body_u16(&trap_fn, 512);
    trap_fn.body[2] = 0x7F;
    uint8_t trap_fn_h[32];
    fails += check_cond(ingest(&trap_fn, trap_fn_h) >= 0, "S4: ingest trap_fn");

    Cell inp = {0};
    inp.body[2] = 'Z';
    uint8_t inp_h[32];
    fails += check_cond(ingest(&inp, inp_h) >= 0, "S4: ingest inp");

    Cell task = {0};
    memcpy(task.left, trap_fn_h, 32);
    memcpy(task.right, inp_h, 32);
    set_body_u16(&task, 512);
    uint8_t root_h[32];
    fails += check_cond(ingest(&task, root_h) >= 0, "S4: ingest task");

    /* Ingest ancestor_b only; keep champ zeroed -> resolve(champ) == -1 */
    Cell b_cell = {0};
    set_body_u16(&b_cell, 512);
    uint8_t code_b[] = {0x01, 0x00, 0x02, 0x05, 0x00};
    memcpy(&b_cell.body[2], code_b, sizeof(code_b));
    fails += check_cond(ingest(&b_cell, ancestor_b) >= 0, "S4: ingest ancestor_b");

    uint8_t zero_champ[32] = {0};
    flow(node_sock, &peer_sa, root_h, zero_champ);

    /* Non-zero missing champ must emit a 32-byte hash request without touching arena[-1] */
    uint8_t missing_champ[32];
    memset(missing_champ, 0x55, 32);
    flow(node_sock, &peer_sa, root_h, missing_champ);
    uint8_t req_buf[64];
    ssize_t rn = recvfrom(peer_sock, req_buf, sizeof(req_buf), 0, NULL, NULL);
    fails += check_cond(rn == 32 && memcmp(req_buf, missing_champ, 32) == 0,
                        "S4: missing non-zero champ requested cleanly");

    /* Now make champ valid and ancestor_b missing -> must also stay clean */
    uint8_t saved_b[32];
    memcpy(saved_b, ancestor_b, 32);
    memset(ancestor_b, 0x77, 32);
    flow(node_sock, &peer_sa, root_h, saved_b);
    rn = recvfrom(peer_sock, req_buf, sizeof(req_buf), 0, NULL, NULL);
    fails += check_cond(rn == 32 && memcmp(req_buf, ancestor_b, 32) == 0,
                        "S4: missing ancestor_b requested cleanly");

    /* S2: Fill all ARENA_CAP slots; verify ingest fails closed without modifying out_h or publishing dangling hashes */
    reset_arena();
    /* First ingest a valid winning single-strand task & its function/input cells */
    Cell win_fn = {0};
    set_body_u16(&win_fn, 512);
    memcpy(&win_fn.body[2], code_b, sizeof(code_b));
    uint8_t win_fn_h[32];
    fails += check_cond(ingest(&win_fn, win_fn_h) >= 0, "S2: ingest win_fn");

    Cell win_in = {0};
    win_in.body[2] = 'Q';
    uint8_t win_in_h[32];
    fails += check_cond(ingest(&win_in, win_in_h) >= 0, "S2: ingest win_in");

    Cell win_task = {0};
    memcpy(win_task.left, win_fn_h, 32);
    memcpy(win_task.right, win_in_h, 32);
    set_body_u16(&win_task, 512);
    uint8_t win_root[32];
    fails += check_cond(ingest(&win_task, win_root) >= 0, "S2: ingest win_task");

    int filled = 3;
    for (int i = 0; filled < ARENA_CAP; i++) {
        Cell filler = {0};
        filler.body[0] = (uint8_t)(i & 0xFF);
        filler.body[1] = (uint8_t)((i >> 8) & 0xFF);
        filler.body[2] = (uint8_t)((i >> 16) & 0xFF);
        filler.body[3] = 0xA5;
        int sl = ingest(&filler, NULL);
        if (sl >= 0) filled++;
    }

    Cell overflow_cell = {0};
    overflow_cell.body[3] = 0x5A;
    uint8_t sentinel[32];
    memset(sentinel, 0xBB, 32);
    uint8_t expected_sentinel[32];
    memset(expected_sentinel, 0xBB, 32);
    fails += check_cond(ingest(&overflow_cell, sentinel) == -1, "S2: full arena ingest returns -1");
    fails += check_cond(memcmp(sentinel, expected_sentinel, 32) == 0,
                        "S2: failed ingest does not emit dangling out_h");
    uint8_t overflow_h[32];
    hash_cell(&overflow_cell, overflow_h);
    fails += check_cond(resolve(overflow_h) == -1, "S2: overflow cell not stored");
    fails += check_cond(sacred_splice(&win_fn, &win_fn, overflow_h, sentinel) == -1,
                        "S2: sacred_splice returns -1 on full arena");
    fails += check_cond(memcmp(sentinel, expected_sentinel, 32) == 0,
                        "S2: failed sacred_splice leaves out_fork untouched");

    /* Run flow() on full arena: winning output cannot be ingested so peer must receive 0 bytes & root unchanged */
    uint8_t root_before[32];
    memcpy(root_before, win_root, 32);
    uint8_t champ_full[32];
    memcpy(champ_full, win_fn_h, 32);
    flow(node_sock, &peer_sa, win_root, champ_full);
    fails += check_cond(memcmp(win_root, root_before, 32) == 0,
                        "S2: full-arena flow keeps root resident");
    fails += check_cond(resolve(win_root) >= 0, "S2: root remains resolvable");
    uint8_t peer_rx[sizeof(Cell)];
    ssize_t prx = recvfrom(peer_sock, peer_rx, sizeof(peer_rx), 0, NULL, NULL);
    fails += check_cond(prx < 0, "S2: full-arena flow publishes no dangling cell to peer");

    /* UDP datagram arrival on aligned-copied Cell works cleanly when space exists */
    reset_arena();
    struct sockaddr_in node_sa;
    memset(&node_sa, 0, sizeof(node_sa));
    node_sa.sin_family = AF_INET;
    node_sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    node_sa.sin_port = htons(node_port);
    fails += check_cond(
        sendto(peer_sock, &aligned, sizeof(Cell), 0, (struct sockaddr *)&node_sa, sizeof(node_sa)) == (ssize_t)sizeof(Cell),
        "S3: send 1088B datagram to v1 node");
    uint8_t empty_root[32] = {0}, empty_champ[32] = {0};
    flow(node_sock, &peer_sa, empty_root, empty_champ);
    fails += check_cond(memcmp(empty_root, h_aligned, 32) == 0 && resolve(empty_root) >= 0,
                        "S3: UDP datagram ingested via aligned Cell copy");

    close(node_sock);
    close(peer_sock);
    if (fails == 0) printf("PASS test_c11_hardening (v1_genesis: S2, S3, S4)\n");
    return fails ? 1 : 0;
}

#elif defined(TEST_SUBSTRATE_V2)
#include "v2_adaptive_loop.c"

static int check_cond(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "[FAIL v2] %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    (void)ancestor_a;
    Cell c = {0};
    set_body_u16(&c, 512);
    uint8_t code[] = {0x01, 0x00, 0x02, 0x05, 0x00};
    memcpy(&c.body[CODE_START], code, sizeof(code));

    uint8_t odd_buf[sizeof(Cell) + 1];
    odd_buf[0] = 0xAA;
    memcpy(odd_buf + 1, &c, sizeof(Cell));
    uint8_t h1[32], h2[32];
    hash_cell(&c, h1);
    hash_cell((const Cell *)(odd_buf + 1), h2);
    fails += check_cond(memcmp(h1, h2, 32) == 0, "S3(v2): odd-offset hash_cell parity");

    /* Exercise v2 flow() on loopback UDP datagram (aligned copy path) */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (check_cond(s >= 0, "S3(v2): socket udp") != 0) return 1;
    int fl = fcntl(s, F_GETFL, 0);
    if (check_cond(fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0, "S3(v2): nonblock udp") != 0) {
        close(s);
        return 1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (check_cond(bind(s, (struct sockaddr *)&sa, sizeof(sa)) == 0, "S3(v2): bind udp") != 0) {
        close(s);
        return 1;
    }
    socklen_t sal = sizeof(sa);
    if (check_cond(getsockname(s, (struct sockaddr *)&sa, &sal) == 0, "S3(v2): getsockname") != 0) {
        close(s);
        return 1;
    }
    fails += check_cond(
        sendto(s, &c, sizeof(Cell), 0, (struct sockaddr *)&sa, sizeof(sa)) == (ssize_t)sizeof(Cell),
        "S3(v2): sendto");
    uint8_t v2_root[32] = {0}, v2_champ[32] = {0};
    flow(s, &sa, v2_root, v2_champ);
    fails += check_cond(memcmp(v2_root, h1, 32) == 0 && resolve(v2_root) >= 0, "S3(v2): flow ingested");
    close(s);

    int filled = 1;
    for (int i = 0; filled < ARENA_CAP; i++) {
        Cell f = {0};
        f.body[0] = (uint8_t)(i & 0xFF);
        f.body[1] = (uint8_t)((i >> 8) & 0xFF);
        f.body[2] = 0x33;
        if (ingest(&f, NULL) >= 0) filled++;
    }
    Cell extra = {0};
    extra.body[2] = 0xCC;
    uint8_t sent[32];
    memset(sent, 0xDD, 32);
    uint8_t exp_sent[32];
    memset(exp_sent, 0xDD, 32);
    fails += check_cond(ingest(&extra, sent) == -1, "S2(v2): full arena returns -1");
    fails += check_cond(memcmp(sent, exp_sent, 32) == 0, "S2(v2): out_h untouched on full arena");
    if (fails == 0) printf("PASS test_c11_hardening (v2_adaptive_loop: S2, S3)\n");
    return fails ? 1 : 0;
}

#elif defined(TEST_SUBSTRATE_V3)
#include "v3_evogenesis.c"

static int check_cond(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "[FAIL v3] %s\n", msg);
        return 1;
    }
    return 0;
}

static int bind_loopback_udp(uint16_t *out_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) {
        close(s);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, (struct sockaddr *)&addr, &alen) < 0) {
        close(s);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);
    return s;
}

static void reset_v3_arena(void) {
    memset(arena, 0, sizeof(arena));
    memset(names, 0, sizeof(names));
    memset(occupied, 0, sizeof(occupied));
}

int main(void) {
    int fails = 0;

    /* S1: Feed identical 1,088-byte cell twice over non-blocking UDP, then a distinct third cell */
    reset_v3_arena();
    uint16_t recv_port = 0, send_port = 0;
    udp_sock = bind_loopback_udp(&recv_port);
    int tx_sock = bind_loopback_udp(&send_port);
    if (check_cond(udp_sock >= 0 && tx_sock >= 0, "S1: loopback UDP sockets created") != 0) {
        if (tx_sock >= 0) close(tx_sock);
        if (udp_sock >= 0) {
            close(udp_sock);
            udp_sock = -1;
        }
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(recv_port);

    Cell c1;
    build_seed_cell(&c1);
    uint8_t exp_h1[32];
    hash_cell(&c1, exp_h1);

    /* Packet 1: genuinely new cell */
    fails += check_cond(
        sendto(tx_sock, &c1, CELL_BYTES, 0, (struct sockaddr *)&dst, sizeof(dst)) == CELL_BYTES,
        "S1: send first cell");
    uint8_t got_h[32];
    memset(got_h, 0xEE, 32);
    bool got_new = false;
    poll_network(got_h, &got_new);
    fails += check_cond(got_new == true, "S1: first arrival sets got_new_cell = true");
    fails += check_cond(memcmp(got_h, exp_h1, 32) == 0, "S1: stored hash matches hash_cell(&c1)");
    fails += check_cond(arena_lookup(exp_h1) >= 0, "S1: cell resident in arena");

    /* Packet 2: exact duplicate of resident cell -> got_new_cell MUST be reset to false */
    fails += check_cond(
        sendto(tx_sock, &c1, CELL_BYTES, 0, (struct sockaddr *)&dst, sizeof(dst)) == CELL_BYTES,
        "S1: send duplicate cell");
    memset(got_h, 0xDD, 32);
    got_new = true;
    poll_network(got_h, &got_new);
    fails += check_cond(got_new == false, "S1: duplicate arrival resets got_new_cell to false");

    /* Packet 3: distinct cell -> got_new_cell MUST become true again */
    Cell c2 = c1;
    c2.body[CODE_START] = OP_ONE;
    uint8_t exp_h2[32];
    hash_cell(&c2, exp_h2);
    fails += check_cond(
        sendto(tx_sock, &c2, CELL_BYTES, 0, (struct sockaddr *)&dst, sizeof(dst)) == CELL_BYTES,
        "S1: send third distinct cell");
    got_new = false;
    poll_network(got_h, &got_new);
    fails += check_cond(got_new == true, "S1: distinct third cell sets got_new_cell = true");
    fails += check_cond(memcmp(got_h, exp_h2, 32) == 0, "S1: third cell hash matches");

    /* S2: Insert ARENA_CAP + 1 distinct cells; verify fail-closed full arena preserves every resident */
    reset_v3_arena();
    Cell seed_a;
    build_seed_cell(&seed_a);
    uint8_t seed_h[32];
    fails += check_cond(arena_put(&seed_a, seed_h) >= 0, "S2: store seed_a");

    int count = 1;
    for (int i = 0; count < ARENA_CAP; i++) {
        Cell c;
        memset(&c, 0, sizeof(c));
        c.body[0] = (uint8_t)(i & 0xFF);
        c.body[1] = (uint8_t)((i >> 8) & 0xFF);
        c.body[2] = 0xA5;
        uint8_t h[32];
        int sl = arena_put(&c, h);
        if (sl >= 0) {
            uint8_t verify_h[32];
            hash_cell(&arena[sl], verify_h);
            if (memcmp(verify_h, h, 32) != 0) {
                fails += check_cond(0, "S2: stored slot bytes hash matches name");
                break;
            }
            count++;
        }
    }

    /* ARENA_CAP + 1 distinct cell MUST return -1 and leave out_hash & all residents untouched */
    Cell extra;
    memset(&extra, 0, sizeof(extra));
    extra.body[2] = 0x5A;
    uint8_t extra_out[32];
    memset(extra_out, 0xCC, 32);
    uint8_t exp_unchanged[32];
    memset(exp_unchanged, 0xCC, 32);
    bool ins_flag = true;
    int extra_slot = arena_put_status(&extra, extra_out, &ins_flag);
    fails += check_cond(extra_slot == -1, "S2: ARENA_CAP+1 insert returns -1");
    fails += check_cond(ins_flag == false, "S2: ARENA_CAP+1 inserted_out is false");
    fails += check_cond(memcmp(extra_out, exp_unchanged, 32) == 0,
                        "S2: ARENA_CAP+1 leaves out_hash untouched");

    /* Duplicate of resident seed_a still succeeds (returns existing slot, inserted=false) */
    uint8_t dup_h[32];
    bool dup_ins = true;
    int dup_slot = arena_put_status(&seed_a, dup_h, &dup_ins);
    fails += check_cond(dup_slot >= 0 && dup_ins == false && memcmp(dup_h, seed_h, 32) == 0,
                        "S2: resident duplicate still resolves on full arena");

    /* Every single previously inserted cell must remain resolvable and hash-verified */
    int lost = 0;
    if (arena_lookup(seed_h) < 0) lost++;
    for (int i = 0; i < ARENA_CAP - 1; i++) {
        Cell c;
        memset(&c, 0, sizeof(c));
        c.body[0] = (uint8_t)(i & 0xFF);
        c.body[1] = (uint8_t)((i >> 8) & 0xFF);
        c.body[2] = 0xA5;
        uint8_t h[32];
        hash_cell(&c, h);
        int sl = arena_lookup(h);
        if (sl < 0) {
            lost++;
        } else {
            uint8_t vh[32];
            hash_cell(&arena[sl], vh);
            if (memcmp(vh, h, 32) != 0) lost++;
        }
    }
    fails += check_cond(lost == 0, "S2: zero residents lost after full-arena rejection");

    /* sacred_splice on full arena fails closed (returns false) */
    Cell child_out;
    uint8_t ent[32] = {1, 2, 3};
    fails += check_cond(sacred_splice(&seed_a, &seed_a, seed_h, seed_h, ent, &child_out) == false,
                        "S2: sacred_splice returns false on full arena");

    /* poll_network on full arena ignores unstoreable new cell without setting got_new_cell */
    fails += check_cond(
        sendto(tx_sock, &extra, CELL_BYTES, 0, (struct sockaddr *)&dst, sizeof(dst)) == CELL_BYTES,
        "S2: send new cell to full arena");
    got_new = false;
    poll_network(got_h, &got_new);
    fails += check_cond(got_new == false, "S2: full-arena gossip does not report stored new cell");

    /* Exercise remaining v3 helpers under ASan+UBSan */
    int64_t err_out = 0;
    fails += check_cond(score_anchors(&c1, &err_out) >= 1, "S1(v3): score_anchors clean");
    Cell mut_out;
    uint8_t mut_cod = 0;
    uint8_t ent_m[32] = {7, 1, 2, 3};
    (void)mutate_cell(&c1, exp_h1, ent_m, &mut_out, &mut_cod);
    adapt_prior(&c1, mut_cod, true);

    close(tx_sock);
    close(udp_sock);
    udp_sock = -1;
    if (fails == 0) printf("PASS test_c11_hardening (v3_evogenesis: S1, S2)\n");
    return fails ? 1 : 0;
}

#elif defined(TEST_SUBSTRATE_V4)
#include "v4_unigenesis.c"

static int check_cond(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "[FAIL v4] %s\n", msg);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    srand(42u);
    /* S3(v4): Odd-offset Cell header read/write under UBSan */
    uint8_t odd_buf[sizeof(Cell) + 1];
    odd_buf[0] = 0x77;
    Cell *unaligned = (Cell *)(odd_buf + 1);
    seed_cell(unaligned, 13ULL);
    fails += check_cond(verify_triangular_anchor(unaligned) == 1,
                        "S3(v4): odd-offset triangular anchor valid");
    int32_t fit = evaluate_cell(unaligned);
    fails += check_cond(fit > 0 && get_hdr_i32(unaligned, HDR_FITNESS) == fit,
                        "S3(v4): odd-offset evaluate_cell and get_hdr_i32 clean");
    mutate_cell(unaligned);
    fails += check_cond(get_hdr_i32(unaligned, HDR_GENERATION) == 1,
                        "S3(v4): odd-offset mutate_cell increments generation cleanly");

    /* Untrusted migrant weights at INT32_MAX / INT32_MIN must not trigger signed overflow UB */
    for (int i = 0; i < 8; i++) set_hdr_i32(unaligned, HDR_WEIGHTS + i, INT32_MAX);
    mutate_cell(unaligned);
    adapt_prior(unaligned, 0, 100, 0);
    fails += check_cond(get_hdr_i32(unaligned, HDR_WEIGHTS) == 1000,
                        "S3(v4): INT32_MAX migrant weights clamp cleanly without UB");
    for (int i = 0; i < 8; i++) set_hdr_i32(unaligned, HDR_WEIGHTS + i, INT32_MIN);
    mutate_cell(unaligned);
    adapt_prior(unaligned, 100, 0, 1);
    fails += check_cond(get_hdr_i32(unaligned, HDR_WEIGHTS + 1) == 10,
                        "S3(v4): INT32_MIN migrant weights clamp cleanly without UB");
    if (fails == 0) printf("PASS test_c11_hardening (v4_unigenesis: S3)\n");
    return fails ? 1 : 0;
}

#else
#error "Define one of TEST_SUBSTRATE_V1..TEST_SUBSTRATE_V4"
#endif
