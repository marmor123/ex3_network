/*
 * Lab #2 — Verbs throughput benchmark (single source, server and client
 * roles decided by argv: no hostname argument = server, hostname = client).
 *
 * Stage 5 (T5): the streaming data path (ADR-0002) replacing the naive
 * one. The client runs the 21-size sweep (1 B..1 MB, powers of two) with
 * ex1's converged counts table. Per size the timed batch of RDMA WRITEs
 * (no warmup — ex1's warmup counts measured no benefit, and their wire
 * time sits inside the measured window): K WRs posted per ibv_post_send
 * as a linked list (W=256, K=64, fixed — no options), only the K-th WR of
 * the stream signaled — one CQE per K WRs, so completions are accounted
 * in exact multiples of K (RC in-order)
 * — reclaimed only while the window is full (refill-never-empty, the SQ
 * never empties), and messages ≤ max_inline_data sent with IBV_SEND_INLINE.
 * The clock (CLOCK_MONOTONIC) starts at the first timed post and stops at
 * the ack-receive completion (ADR-0003); each size prints an ex1-identical
 * "size\t%.2f\tunit" line with auto-scaled bps→Gbps units. The server's
 * only data-path role is absorbing the WRITEs into its registered buffer;
 * it just acks each done.
 *
 * The control protocol (T3): per size one done SEND (client, signaled)
 * and one ack SEND (server, 8-byte inline carrying the sequence counter),
 * both riding the data QP (ADR-0001) over the 32-deep control receive
 * pool posted at init, never refreshed.
 *
 * Device init and handshake: the two 1 MB buffer registrations (the
 * server's with remote-write permission); QP create → init → RTR → RTS
 * with the port's active MTU; the TCP exchange of LID/QPN/PSN plus the
 * server's buffer addr/rkey.
 *
 * Adapted from the assignment's bw_template.c: the socket exchange is the
 * template's, extended with the server's buffer address and rkey; the QP
 * lifecycle (INIT/RTR/RTS, rnr_retry/retry_cnt, access flags) is the
 * template's.
 */

/* asprintf and the srand48/lrand48 family are GNU/SVID extensions: newer
 * glibc exposes them by default, the course nodes' older glibc only with
 * _GNU_SOURCE. Must precede every system header. */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <inttypes.h>

#include <infiniband/verbs.h>

/* One 1 MB buffer per side, registered once and never modified after init,
 * so there is no buffer-reuse hazard at full window depth (ADR-0002). */
#define BUFFER_SIZE (1u << 20)

/* Control receive pool (ADR-0001): 32 receives per side, posted once at
 * init, all pointing at one control area, never refreshed. Covers the 21
 * per-direction control messages of a full sweep. */
#define CTRL_POOL_DEPTH 32
#define CTRL_MSG_LEN 64

/* The number of control exchanges per direction: one done/ack pair per
 * size of the sweep (2^0..2^20), which also proves the 32-deep control
 * receive pool covers a full sweep. */
#define SWEEP_SIZES 21

/* The ex1 converged counts table, verbatim: one entry per size of the
 * sweep (2^0..2^20). MSG_COUNTS from ex1's convergence experiments
 * (throughput variance < 1% between doubled counts). */
static const uint64_t MSG_COUNTS[SWEEP_SIZES] = {
        1310720, 81920, 655360, 163840, 327680, /* 1B 2B 4B 8B 16B */
        20480, 81920, 81920, 40960, 20480,      /* 32B 64B 128B 256B 512B */
        20480, 20480, 20480, 2560, 2560,        /* 1KB 2KB 4KB 8KB 16KB */
        2560, 640, 320, 160, 160,               /* 32KB 64KB 128KB 256KB 512KB */
        80                                       /* 1MB */
};

/* Control messages: 8 bytes each — a fixed tag plus the sequence counter
 * (the size index 0..20). The ack carries the received done verbatim, so
 * a tag or sequence mismatch means the exchange desynchronized. */
#define BW_CTRL_TAG 0x4354524cu
struct bw_ctrl_msg {
    uint32_t tag;
    uint32_t seq;
};

/* The control message must always fit one inline send. */
typedef char bw_ctrl_msg_size[(sizeof (struct bw_ctrl_msg) == 8) ? 1 : -1];

/* A control wait (the done on the server, the ack on the client, the
 * ack-send on the server) has a deadline: the peer may have died, and a
 * hung busy poll would tie up a course node until the verify script's
 * own timeout fires. */
#define CTRL_POLL_TIMEOUT_SEC 10

/* The run's fixed configuration — the option-era -r/-k/-p/-i are gone:
 * pipe depth W, signal interval K (only the K-th WR of the stream is
 * signaled), the device's IB port 1, and the TCP handshake port. */

/* The SQ depth is requested as W + K — the pipe depth plus one signal
 * interval, exactly the deepest the refill lets the SQ get: its single
 * trigger (outstanding + K ≥ sq_depth) holds the pipe at W outstanding
 * when the grant matches the request, and at sq_depth - K if the
 * max_qp_wr clamp cuts the grant short. */
#define WINDOW 256
#define SIGNAL_INTERVAL 64
#define IB_PORT 1
#define HANDSHAKE_PORT 18515

/* K ≤ W keeps the pipe (W outstanding) at least one full K-WR list deep,
 * so the refill never fires before a complete list is in flight. The
 * option-era runtime guard is gone; the fixed constants make the check
 * provable at compile time. */
typedef char bw_params_sane[SIGNAL_INTERVAL <= WINDOW ? 1 : -1];

/* The largest max_inline_data tried at QP creation. mlx4 — the course
 * hardware — rejects QP creation when the declared value exceeds what its
 * WQEs can carry, and no portable query exposes that ceiling on every
 * stack, so the declaration is stepped down (bw_init_ctx) until creation
 * succeeds. The value the QP was actually created with is read back via
 * ibv_query_qp; that read-back is the runtime max_inline_data the data
 * path uses. */
#define MAX_INLINE_DATA_DECLARE 1024

/* Fixed-size handshake message, both directions. The client's message
 * carries only the first three fields (addr/rkey are the server's to give). */
#define DEST_MSG_LEN 128

/* The handshake's wire format, kept in one place so the send and parse
 * sides cannot drift: lid:qpn:psn, plus :addr:rkey on the server. */
#define DEST_FMT         "%04x:%06x:%06x"
#define DEST_FMT_SERVER  DEST_FMT ":%" PRIx64 ":%x"
#define DEST_FMT_PARSE   "%x:%x:%x:%" SCNx64 ":%x"

enum {
    /* Control receive: the done on the server, the ack on the client. All
     * 32 receives of the control receive pool share one wr_id — they all
     * point at the same control area, so which receive completed never
     * matters. */
    BW_RECV_WRID = 1,
    /* The client's done SEND, always signaled. */
    BW_SEND_DONE_WRID,
    /* The server's ack SEND, always signaled so the server consumes its
     * completion before exiting. */
    BW_SEND_ACK_WRID,
    /* Data WRITEs; one shared wr_id keeps their completions
     * distinguishable from the control messages. */
    BW_DATA_WRID,
};

static int page_size;

struct bw_context {
    struct ibv_context		*context;
    struct ibv_pd		*pd;
    struct ibv_mr		*mr;      /* the 1 MB buffer */
    struct ibv_mr		*ctrl_mr; /* the control receive area */
    struct ibv_cq		*cq;
    struct ibv_qp		*qp;
    void			*buf;     /* the 1 MB buffer */
    void			*ctrl_buf;/* the control receive area */
    uint32_t		 max_inline_data; /* the QP's negotiated max_inline_data */
    uint32_t		 sq_depth;      /* the QP's negotiated max_send_wr */
    struct ibv_port_attr	 portinfo;
};

struct bw_dest {
    int lid;
    int qpn;
    int psn;
    /* Server side only: buffer address and rkey for the client's RDMA
     * WRITEs. Zero on the client (the server never touches client memory). */
    uint64_t buf_addr;
    uint32_t rkey;
};

/* Loop until len bytes move or the stream ends: the handshake messages are
 * fixed-size, and a short read would break the parse. */
static int bw_read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, (char *) buf + got, len - got);
        if (n <= 0)
            return 0;
        got += n;
    }
    return 1;
}

static int bw_write_full(int fd, const void *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, (const char *) buf + sent, len - sent);
        if (n <= 0)
            return 0;
        sent += n;
    }
    return 1;
}

static struct bw_dest *bw_parse_dest(const char *msg, int expect_addr)
{
    struct bw_dest *dest = calloc(1, sizeof *dest);
    int n;

    if (!dest)
        return NULL;

    /* The server's message carries all five fields; the client's carries the
     * first three (addr/rkey stay zero). The client expects the server's
     * addr/rkey — its RDMA WRITEs land there — so it requires all five; a
     * truncated server message must not pass with addr/rkey zero. */
    n = sscanf(msg, DEST_FMT_PARSE,
               &dest->lid, &dest->qpn, &dest->psn,
               &dest->buf_addr, &dest->rkey);
    if (n < 3 || (expect_addr && n < 5)) {
        free(dest);
        return NULL;
    }

    return dest;
}

static int bw_connect_qp(struct bw_context *ctx, int port, int my_psn,
                         struct bw_dest *dest)
{
    struct ibv_qp_attr attr = {
            .qp_state		= IBV_QPS_RTR,
            .path_mtu		= ctx->portinfo.active_mtu,
            .dest_qp_num		= dest->qpn,
            .rq_psn			= dest->psn,
            .max_dest_rd_atomic	= 1,
            .min_rnr_timer		= 12,
            .ah_attr		= {
                    .is_global	= 0,
                    .dlid		= dest->lid,
                    .sl		= 0,
                    .src_path_bits	= 0,
                    .port_num	= port
            }
    };

    if (ibv_modify_qp(ctx->qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_AV                 |
            IBV_QP_PATH_MTU           |
            IBV_QP_DEST_QPN           |
            IBV_QP_RQ_PSN             |
            IBV_QP_MAX_DEST_RD_ATOMIC |
            IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        return 1;
    }

    attr.qp_state	    = IBV_QPS_RTS;
    attr.timeout	    = 14;
    attr.retry_cnt	    = 7;
    attr.rnr_retry	    = 7;
    attr.sq_psn	    = my_psn;
    attr.max_rd_atomic  = 1;
    if (ibv_modify_qp(ctx->qp, &attr,
            IBV_QP_STATE              |
            IBV_QP_TIMEOUT            |
            IBV_QP_RETRY_CNT          |
            IBV_QP_RNR_RETRY          |
            IBV_QP_SQ_PSN             |
            IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        return 1;
    }

    return 0;
}

static struct bw_dest *bw_exch_dest_client(const char *servername, int port,
                                           const struct bw_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[DEST_MSG_LEN];
    int n;
    int sockfd = -1;
    struct bw_dest *rem_dest = NULL;

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(servername, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        /* No server listening: fail silently — exit non-zero with nothing
           printed (T1 acceptance criterion). */
        return NULL;
    }

    memset(msg, 0, sizeof msg);
    sprintf(msg, DEST_FMT, my_dest->lid, my_dest->qpn, my_dest->psn);
    if (!bw_write_full(sockfd, msg, sizeof msg)) {
        fprintf(stderr, "Couldn't send local address\n");
        goto out;
    }

    if (!bw_read_full(sockfd, msg, sizeof msg)) {
        perror("client read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }

    /* The server keeps the socket open until we signal receipt, so this
     * must go out before we close. */
    if (!bw_write_full(sockfd, "ready", sizeof "ready")) {
        perror("client write");
        goto out;
    }

    rem_dest = bw_parse_dest(msg, 1);
    if (!rem_dest) {
        fprintf(stderr, "Couldn't parse remote address\n");
        goto out;
    }

out:
    close(sockfd);
    return rem_dest;
}

static struct bw_dest *bw_exch_dest_server(struct bw_context *ctx,
                                           int ib_port, int port,
                                           const struct bw_dest *my_dest)
{
    struct addrinfo *res, *t;
    struct addrinfo hints = {
            .ai_flags    = AI_PASSIVE,
            .ai_family   = AF_INET,
            .ai_socktype = SOCK_STREAM
    };
    char *service;
    char msg[DEST_MSG_LEN];
    int n;
    int sockfd = -1, connfd;
    struct bw_dest *rem_dest = NULL;

    if (asprintf(&service, "%d", port) < 0)
        return NULL;

    n = getaddrinfo(NULL, service, &hints, &res);

    if (n < 0) {
        fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
        free(service);
        return NULL;
    }

    for (t = res; t; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd >= 0) {
            n = 1;

            setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

            if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
                break;
            close(sockfd);
            sockfd = -1;
        }
    }

    freeaddrinfo(res);
    free(service);

    if (sockfd < 0) {
        fprintf(stderr, "Couldn't listen to port %d\n", port);
        return NULL;
    }

    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, 0);
    close(sockfd);
    if (connfd < 0) {
        fprintf(stderr, "accept() failed\n");
        return NULL;
    }

    if (!bw_read_full(connfd, msg, sizeof msg)) {
        perror("server read");
        fprintf(stderr, "Couldn't read remote address\n");
        goto out;
    }

    rem_dest = bw_parse_dest(msg, 0);
    if (!rem_dest) {
        fprintf(stderr, "Couldn't parse remote address\n");
        goto out;
    }

    if (bw_connect_qp(ctx, ib_port, my_dest->psn, rem_dest)) {
        fprintf(stderr, "Couldn't connect to remote QP\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    /* Send our address plus the buffer addr/rkey the client needs for its
     * RDMA WRITEs. */
    memset(msg, 0, sizeof msg);
    sprintf(msg, DEST_FMT_SERVER,
            my_dest->lid, my_dest->qpn, my_dest->psn,
            my_dest->buf_addr, my_dest->rkey);
    if (!bw_write_full(connfd, msg, sizeof msg)) {
        fprintf(stderr, "Couldn't send local address\n");
        free(rem_dest);
        rem_dest = NULL;
        goto out;
    }

    /* Final beat: the client signals it has our address with "ready" and
     * closes. The template leaves this read unchecked; the message is
     * short, so check the count, not the message size. */
    {
        char ready[sizeof "ready"];

        if (!bw_read_full(connfd, ready, sizeof ready)) {
            perror("server read");
            free(rem_dest);
            rem_dest = NULL;
            goto out;
        }
    }

out:
    close(connfd);
    return rem_dest;
}

static struct bw_context *bw_init_ctx(struct ibv_device *ib_dev, int port,
                                      int is_server)
{
    struct bw_context *ctx;
    struct ibv_device_attr dev_attr;
    uint32_t max_send_wr;
    uint32_t max_recv_wr;

    ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;

    ctx->buf = malloc(roundup(BUFFER_SIZE, page_size));
    if (!ctx->buf) {
        fprintf(stderr, "Couldn't allocate work buf.\n");
        return NULL;
    }

    ctx->ctrl_buf = malloc(CTRL_MSG_LEN);
    if (!ctx->ctrl_buf) {
        fprintf(stderr, "Couldn't allocate control buf.\n");
        return NULL;
    }

    ctx->context = ibv_open_device(ib_dev);
    if (!ctx->context) {
        fprintf(stderr, "Couldn't get context for %s\n",
                ibv_get_device_name(ib_dev));
        return NULL;
    }

    /* The advertised per-QP WR limits: mlx4 caps the WQEs a QP may hold,
     * and a declaration above the device's max_qp_wr fails QP creation. */
    if (ibv_query_device(ctx->context, &dev_attr)) {
        fprintf(stderr, "Couldn't query device attributes\n");
        return NULL;
    }
    max_send_wr = WINDOW + SIGNAL_INTERVAL;
    max_recv_wr = CTRL_POOL_DEPTH;
    if (max_send_wr > (uint32_t) dev_attr.max_qp_wr)
        max_send_wr = dev_attr.max_qp_wr;
    if (max_recv_wr > (uint32_t) dev_attr.max_qp_wr)
        max_recv_wr = dev_attr.max_qp_wr;

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (!ctx->pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        return NULL;
    }

    /* The server's buffer must accept remote writes (the client's RDMA
     * WRITEs land here); the client's is only read by its own HCA. */
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, BUFFER_SIZE,
                         IBV_ACCESS_LOCAL_WRITE |
                         (is_server ? IBV_ACCESS_REMOTE_WRITE : 0));
    if (!ctx->mr) {
        fprintf(stderr, "Couldn't register MR\n");
        return NULL;
    }

    ctx->ctrl_mr = ibv_reg_mr(ctx->pd, ctx->ctrl_buf, CTRL_MSG_LEN,
                              IBV_ACCESS_LOCAL_WRITE);
    if (!ctx->ctrl_mr) {
        fprintf(stderr, "Couldn't register control MR\n");
        return NULL;
    }

    ctx->cq = ibv_create_cq(ctx->context, max_send_wr + max_recv_wr,
                            NULL, NULL, 0);
    if (!ctx->cq) {
        fprintf(stderr, "Couldn't create CQ\n");
        return NULL;
    }

    {
        int try_inline;

        /* The inline declaration is stepped down until QP creation
         * succeeds: mlx4 rejects the QP when the declared max_inline_data
         * (plus WQE overhead) exceeds what the hardware accepts, and no
         * portable query exposes that ceiling on every stack. The value
         * the QP was actually created with is read back below; that
         * read-back is the runtime max_inline_data the data path uses. */
        for (try_inline = MAX_INLINE_DATA_DECLARE;; try_inline -= 64) {
            struct ibv_qp_init_attr attr = {
                    .send_cq = ctx->cq,
                    .recv_cq = ctx->cq,
                    .cap     = {
                            .max_send_wr  = max_send_wr,
                            .max_recv_wr  = max_recv_wr,
                            .max_send_sge = 1,
                            .max_recv_sge = 1,
                            .max_inline_data = try_inline
                    },
                    .qp_type = IBV_QPT_RC
            };

            ctx->qp = ibv_create_qp(ctx->pd, &attr);
            if (ctx->qp || try_inline <= 0)
                break;
        }
        if (!ctx->qp) {
            fprintf(stderr,
                    "Couldn't create QP (send_wr %u, recv_wr %u, inline %d): %s\n",
                    max_send_wr, max_recv_wr, try_inline, strerror(errno));
            return NULL;
        }
    }

    {
        /* The runtime max_inline_data: read back what the QP was created
         * with, since the driver may clamp the request. */
        struct ibv_qp_attr attr;
        struct ibv_qp_init_attr init_attr;

        if (ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr)) {
            fprintf(stderr, "Couldn't query QP attributes\n");
            return NULL;
        }
        ctx->max_inline_data = init_attr.cap.max_inline_data;
        ctx->sq_depth = init_attr.cap.max_send_wr;
    }

    {
        struct ibv_qp_attr attr = {
                .qp_state        = IBV_QPS_INIT,
                .pkey_index      = 0,
                .port_num        = port,
                .qp_access_flags = IBV_ACCESS_REMOTE_READ |
                IBV_ACCESS_REMOTE_WRITE
        };

        if (ibv_modify_qp(ctx->qp, &attr,
                IBV_QP_STATE              |
                IBV_QP_PKEY_INDEX         |
                IBV_QP_PORT               |
                IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "Failed to modify QP to INIT\n");
            return NULL;
        }
    }

    return ctx;
}

/* Post the entire control receive pool — never refreshed: each control
 * message consumes one pre-posted receive, and 32 cover a full sweep
 * (ADR-0001, assignment item 3). Returns 0 when all are posted. */
static int bw_post_control_recvs(struct bw_context *ctx)
{
    struct ibv_sge list = {
            .addr	= (uint64_t) ctx->ctrl_buf,
            .length = CTRL_MSG_LEN,
            .lkey	= ctx->ctrl_mr->lkey
    };
    struct ibv_recv_wr wr = {
            .wr_id	    = BW_RECV_WRID,
            .sg_list    = &list,
            .num_sge    = 1,
            .next       = NULL
    };
    struct ibv_recv_wr *bad_wr;
    int i;

    for (i = 0; i < CTRL_POOL_DEPTH; ++i)
        if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
            break;

    return i == CTRL_POOL_DEPTH ? 0 : 1;
}

/* Post one control SEND — the client's done or the server's ack — always
 * signaled so the sender consumes a completion. The message rides inline when
 * the QP's max_inline_data allows it (it always does in practice); the
 * fallback stages it in the registered control area. */
static int bw_post_ctrl_send(struct bw_context *ctx, uint64_t wrid,
                             const struct bw_ctrl_msg *msg)
{
    struct ibv_sge sge = {
            .addr	= (uintptr_t) msg,
            .length = sizeof *msg,
            .lkey	= 0
    };
    struct ibv_send_wr wr = {
            .wr_id	    = wrid,
            .opcode	    = IBV_WR_SEND,
            .send_flags = IBV_SEND_SIGNALED,
            .sg_list    = &sge,
            .num_sge    = 1,
            .next	    = NULL
    };
    struct ibv_send_wr *bad_wr;

    if (ctx->max_inline_data >= (uint32_t) sizeof *msg) {
        wr.send_flags |= IBV_SEND_INLINE;
    } else {
        /* Non-inline: the SEND is DMA-read from a registered buffer, so
         * the message is staged in the control area. */
        memcpy(ctx->ctrl_buf, msg, sizeof *msg);
        sge.addr = (uint64_t) ctx->ctrl_buf;
        sge.lkey = ctx->ctrl_mr->lkey;
    }

    if (ibv_post_send(ctx->qp, &wr, &bad_wr)) {
        fprintf(stderr, "Couldn't post control SEND\n");
        return 1;
    }
    return 0;
}

/* Classify one completion: good status and a wr_id whose bit is set in
 * `allowed`. Prints the error and returns 1 otherwise — the two poll
 * loops share this so their protocol-error reports cannot drift. */
static int bw_wc_bad(struct ibv_wc *wc, uint64_t allowed)
{
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Bad status %s (%d) for wr_id %llu\n",
                ibv_wc_status_str(wc->status), wc->status,
                (unsigned long long) wc->wr_id);
        return 1;
    }
    if (!(allowed & (1ull << wc->wr_id))) {
        fprintf(stderr, "Unexpected completion for wr_id %llu\n",
                (unsigned long long) wc->wr_id);
        return 1;
    }
    return 0;
}

/* Poll the shared CQ until a completion with wr_id `want` arrives.
 * Completions whose wr_id bit is set in `pass` are consumed and ignored —
 * the client passes its done-send and data completions through while
 * waiting for the ack receive. Nothing else may complete, so a bad status
 * or an unexpected wr_id is a protocol error, as is a wait past the
 * deadline. */
static int bw_poll_until(struct bw_context *ctx, uint64_t want,
                         uint64_t pass, struct ibv_wc *wc)
{
    struct timespec deadline;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += CTRL_POLL_TIMEOUT_SEC;

    for (;;) {
        struct timespec now;
        int ne = ibv_poll_cq(ctx->cq, 1, wc);

        if (ne < 0) {
            fprintf(stderr, "poll CQ failed %d\n", ne);
            return 1;
        }
        if (ne == 1) {
            if (bw_wc_bad(wc, pass | (1ull << want)))
                return 1;
            if (wc->wr_id == want)
                return 0;
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            fprintf(stderr,
                    "Timed out after %d s waiting for wr_id %llu\n",
                    CTRL_POLL_TIMEOUT_SEC, (unsigned long long) want);
            return 1;
        }
    }
}

/* Wait for the next control message on the pre-posted control receive
 * pool and verify it: the fixed tag and the expected sequence counter.
 * `pass` is handed to bw_poll_until — the client passes its done-send
 * and data completions through while waiting for the ack. `t_stamp`, when
 * non-NULL, receives CLOCK_MONOTONIC at the completion — the client's t1
 * for this size. */
static int bw_recv_ctrl(struct bw_context *ctx, uint64_t pass,
                        uint32_t seq, const char *kind,
                        struct timespec *t_stamp)
{
    struct ibv_wc wc;
    struct bw_ctrl_msg msg;

    if (bw_poll_until(ctx, BW_RECV_WRID, pass, &wc))
        return 1;

    if (t_stamp)
        clock_gettime(CLOCK_MONOTONIC, t_stamp);

    msg = *(const struct bw_ctrl_msg *) ctx->ctrl_buf;
    if (msg.tag != BW_CTRL_TAG || msg.seq != seq) {
        fprintf(stderr, "%s mismatch: tag 0x%x seq %u, expected seq %u\n",
                kind, msg.tag, msg.seq, seq);
        return 1;
    }

    return 0;
}

/* The client's streaming data-path state for one size: the windowed
 * pipeline (ADR-0002). posted counts every WR of the size's stream so
 * the signal schedule can pick the K-th WRs; outstanding is posted minus
 * the WRs the refill has reclaimed — exactly K per reclaimed CQE,
 * because only K-th WRs are signaled and RC completions are in-order;
 * the final list's remainder is covered by the CQEs the ack wait
 * consumes, never by the refill. Scoped to one size: the ack wait
 * consumes the remaining data and done completions without touching the
 * state, so it must not survive into the next size. */
struct bw_data_state {
    uint64_t posted;
    uint64_t outstanding;
};

/* Refill-never-empty (ADR-0002): once the SQ is as deep as it can be,
 * reclaim only the CQEs that are ready — each data CQE accounts for
 * exactly K WRs, because only the K-th WR of the stream is signaled and
 * RC completions are in-order — then return immediately so the caller
 * reposts; the SQ never empties and the NIC never idles. The single
 * trigger is the SQ depth itself: with the SQ sized W + K (bw_init_ctx),
 * waiting while outstanding + K ≥ sq_depth holds the pipe at W
 * outstanding, and at sq_depth - K if the max_qp_wr clamp granted less.
 * The final list's CQE is never reclaimed here: no list is posted after
 * it, so the refill cannot run again; that CQE stays in the CQ for the
 * ack wait to consume (it precedes the ack, ADR-0003). During the data
 * path only data WRITE completions may be pending here, so anything else
 * is a protocol error; a poll returning 0 only means the last WQEs are
 * still in flight, so the poll is retried. */
static int bw_refill(struct bw_context *ctx, struct bw_data_state *st)
{
    while (st->outstanding + SIGNAL_INTERVAL >= (uint64_t) ctx->sq_depth) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(ctx->cq, 1, &wc);

        if (ne < 0) {
            fprintf(stderr, "poll CQ failed %d\n", ne);
            return 1;
        }
        if (ne == 0)
            continue;

        if (bw_wc_bad(&wc, 1ull << BW_DATA_WRID))
            return 1;
        st->outstanding -= SIGNAL_INTERVAL;
    }
    return 0;
}

/* Post `n` RDMA WRITEs of `size` bytes into the server's registered
 * buffer, as a stream of K-WR linked lists, one per ibv_post_send (the
 * last list takes the remainder). Signal schedule: the K-th WR of the
 * size's stream (t % K == 0) and the stream's final WR — mid-stream
 * lists therefore yield one CQE per K WRs, while the final remainder is
 * accounted exactly by the signaled final WR (in-order RC). Messages ≤
 * max_inline_data ride the WQE inline (IBV_SEND_INLINE); larger ones use
 * the registered buffer. `wrs`/`sges` are the caller's K-deep WR arrays,
 * reused for every list. `final` marks the call that posts the stream's
 * last list (the timed one). */
static int bw_post_writes(struct bw_context *ctx, const struct bw_dest *dest,
                          size_t size, uint64_t n, struct ibv_send_wr *wrs,
                          struct ibv_sge *sges, struct bw_data_state *st,
                          int final)
{
    uint32_t inline_flag =
    (size <= 64 && size <= ctx->max_inline_data)
        ? IBV_SEND_INLINE
        : 0;

    while (n > 0) {
        uint64_t chunk = n < SIGNAL_INTERVAL ? n : SIGNAL_INTERVAL;
        struct ibv_send_wr *bad_wr;
        uint64_t i;

        if (bw_refill(ctx, st))
            return 1;

        for (i = 0; i < chunk; ++i) {
            uint64_t t = st->posted + i + 1; /* this WR's position in the stream */
            uint32_t signal =
                    (t % SIGNAL_INTERVAL == 0) ||
                    (final && n == chunk && i == chunk - 1);

            sges[i] = (struct ibv_sge) {
                    .addr	= (uint64_t) ctx->buf,
                    .length = size,
                    .lkey	= ctx->mr->lkey
            };
            wrs[i] = (struct ibv_send_wr) {
                    .wr_id	    = BW_DATA_WRID,
                    .opcode	    = IBV_WR_RDMA_WRITE,
                    .send_flags = inline_flag |
                                  (signal ? IBV_SEND_SIGNALED : 0),
                    .sg_list    = &sges[i],
                    .num_sge    = 1,
                    .next	    = i + 1 < chunk ? &wrs[i + 1] : NULL
            };
            wrs[i].wr.rdma.remote_addr = dest->buf_addr;
            wrs[i].wr.rdma.rkey = dest->rkey;
        }

        if (ibv_post_send(ctx->qp, &wrs[0], &bad_wr)) {
            fprintf(stderr, "Couldn't post data WRITEs\n");
            return 1;
        }
        st->posted += chunk;
        st->outstanding += chunk;
        n -= chunk;
    }
    return 0;
}

/* Print one result line, byte-identical to ex1: the size, throughput in
 * auto-scaled units (bps → Gbps), and nothing else. */
static void bw_print_result(size_t size, uint64_t count, double elapsed)
{
    double bps = (double) size * (double) count * 8.0 / elapsed;

    if (bps < 1000.0)
        printf("%zu\t%.2f\t%s\n", size, bps, "bps");
    else if (bps < 1000000.0)
        printf("%zu\t%.2f\t%s\n", size, bps / 1000.0, "Kbps");
    else if (bps < 1000000000.0)
        printf("%zu\t%.2f\t%s\n", size, bps / 1000000.0, "Mbps");
    else
        printf("%zu\t%.2f\t%s\n", size, bps / 1000000000.0, "Gbps");
}

/* Client side of the full sweep: per size, the clock starts at the first
 * timed post and stops at the ack-receive completion (ADR-0003) — closed
 * by the done SEND. The done needs one free SQ slot: the refill exits
 * with at most sq_depth - K - 1 WRs outstanding and the final list adds
 * at most K, so at most sq_depth - 1 — the slot is always free. The ack
 * wait passes the data and done-send completions through; the 21 acks
 * consume 21 of the 32 pre-posted control receive pool, never
 * refreshed. */
static int bw_client_bench(struct bw_context *ctx, const struct bw_dest *dest)
{
    /* The K-deep WR arrays, reused for every linked list of the sweep. */
    struct ibv_send_wr *wrs;
    struct ibv_sge *sges;
    uint32_t seq;
    int rc = 1;

    wrs = calloc(SIGNAL_INTERVAL, sizeof *wrs);
    sges = calloc(SIGNAL_INTERVAL, sizeof *sges);
    if (!wrs || !sges) {
        fprintf(stderr, "Couldn't allocate data batch\n");
        goto out;
    }

    for (seq = 0; seq < SWEEP_SIZES; ++seq) {
        size_t size = (size_t) 1 << seq;
        uint64_t count = MSG_COUNTS[seq];
        struct bw_ctrl_msg done = { .tag = BW_CTRL_TAG, .seq = seq };
        struct timespec t0, t1;
        double elapsed;
        struct bw_data_state st = { 0, 0 };

        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (bw_post_writes(ctx, dest, size, count, wrs, sges, &st, 1))
            goto out;

        if (bw_post_ctrl_send(ctx, BW_SEND_DONE_WRID, &done))
            goto out;

        if (bw_recv_ctrl(ctx,
                         (1ull << BW_SEND_DONE_WRID) | (1ull << BW_DATA_WRID),
                         seq, "Ack", &t1))
            goto out;

        elapsed = (double) (t1.tv_sec - t0.tv_sec) +
                  (double) (t1.tv_nsec - t0.tv_nsec) / 1e9;
        bw_print_result(size, count, elapsed);
    }

    rc = 0;
out:
    free(sges);
    free(wrs);
    return rc;
}

/* Server side: poll each done off the pre-posted control receive pool —
 * nothing is ever reposted — verify its sequence counter, and ack it back.
 * The ack's own send completion is consumed before the next done is
 * awaited: it is the guarantee the ack left the HCA. */
static int bw_server_ctrl_exchange(struct bw_context *ctx)
{
    uint32_t seq;

    for (seq = 0; seq < SWEEP_SIZES; ++seq) {
        struct bw_ctrl_msg ack = { .tag = BW_CTRL_TAG, .seq = seq };
        struct ibv_wc wc;

        if (bw_recv_ctrl(ctx, 0, seq, "Done", NULL))
            return 1;

        if (bw_post_ctrl_send(ctx, BW_SEND_ACK_WRID, &ack))
            return 1;

        if (bw_poll_until(ctx, BW_SEND_ACK_WRID, 0, &wc))
            return 1;
    }

    return 0;
}

static int bw_close_ctx(struct bw_context *ctx)
{
    if (ibv_destroy_qp(ctx->qp)) {
        fprintf(stderr, "Couldn't destroy QP\n");
        return 1;
    }

    if (ibv_destroy_cq(ctx->cq)) {
        fprintf(stderr, "Couldn't destroy CQ\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->ctrl_mr)) {
        fprintf(stderr, "Couldn't deregister control MR\n");
        return 1;
    }

    if (ibv_dereg_mr(ctx->mr)) {
        fprintf(stderr, "Couldn't deregister MR\n");
        return 1;
    }

    if (ibv_dealloc_pd(ctx->pd)) {
        fprintf(stderr, "Couldn't deallocate PD\n");
        return 1;
    }

    if (ibv_close_device(ctx->context)) {
        fprintf(stderr, "Couldn't release context\n");
        return 1;
    }

    free(ctx->ctrl_buf);
    free(ctx->buf);
    free(ctx);

    return 0;
}

static void usage(const char *argv0)
{
    printf("Usage:\n");
    printf("  %s            start a server and wait for connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
}

int main(int argc, char *argv[])
{
    struct ibv_device      **dev_list;
    struct ibv_device       *ib_dev;
    struct bw_context       *ctx;
    struct bw_dest          my_dest;
    struct bw_dest         *rem_dest;
    char                    *servername = NULL;

    srand48(getpid() * time(NULL));

    /* The only interface: no hostname starts a server, one hostname or IP
     * connects to it. Everything else — W, K, the handshake port, the
     * device — is fixed at compile time. */
    if (argc == 2)
        servername = strdup(argv[1]);
    else if (argc > 2) {
        usage(argv[0]);
        return 1;
    }

    page_size = sysconf(_SC_PAGESIZE);

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("Failed to get IB devices list");
        return 1;
    }

    /* The first device found — the option-era -d selection is gone. */
    ib_dev = *dev_list;
    if (!ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
    }

    ctx = bw_init_ctx(ib_dev, IB_PORT, !servername);
    if (!ctx)
        return 1;

    /* The whole control receive pool is posted before the handshake, so no
     * control message can ever find the RQ empty (ADR-0001). */
    if (bw_post_control_recvs(ctx)) {
        fprintf(stderr, "Couldn't post control receives\n");
        return 1;
    }

    if (ibv_query_port(ctx->context, IB_PORT, &ctx->portinfo)) {
        fprintf(stderr, "Couldn't get port info\n");
        return 1;
    }

    my_dest.lid = ctx->portinfo.lid;
    if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest.lid) {
        fprintf(stderr, "Couldn't get local LID\n");
        return 1;
    }

    my_dest.qpn = ctx->qp->qp_num;
    my_dest.psn = lrand48() & 0xffffff;

    /* The path MTU comes from the port's active MTU, so large messages use
     * the largest packets the link allows. */
    if (servername)
        rem_dest = bw_exch_dest_client(servername, HANDSHAKE_PORT, &my_dest);
    else {
        /* The server advertises its buffer — the client's RDMA WRITEs land
         * here — and nothing else beyond the template's QP address. */
        my_dest.buf_addr = (uint64_t) ctx->buf;
        my_dest.rkey = ctx->mr->rkey;
        rem_dest = bw_exch_dest_server(ctx, IB_PORT, HANDSHAKE_PORT, &my_dest);
    }

    if (!rem_dest)
        return 1;

    if (servername)
        if (bw_connect_qp(ctx, IB_PORT, my_dest.psn, rem_dest))
            return 1;

    /* The full sweep: the client streams the WRITEs of each size and
     * drives one done SEND per size, the server acks each. Both sides
     * verify every sequence counter. */
    if (servername) {
        if (bw_client_bench(ctx, rem_dest))
            return 1;
    } else {
        if (bw_server_ctrl_exchange(ctx))
            return 1;
    }

    /* All 21 sizes complete; the 21 result lines were printed. */
    {
        int rc = bw_close_ctx(ctx);

        free(rem_dest);
        ibv_free_device_list(dev_list);
        return rc;
    }
}
