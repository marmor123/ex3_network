#define _GNU_SOURCE
#include "pg.h"
#include "pg_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <poll.h>

/* Global process group arguments initialized from CLI */
struct pg_args g_pg_args = {0};

/* ========================================================================= */
/* === MODULE 1: TCP BOOTSTRAP & CLI TOPOLOGY                           === */
/* ========================================================================= */

/* Exact-length stream read helper handling EINTR and short reads */
static int pg_tcp_read_full(int fd, void *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else {
            /* Unexpected EOF before reading requested length */
            return -1;
        }
    }
    return 0;
}

/* Exact-length stream write helper handling EINTR and short writes */
static int pg_tcp_write_full(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, (const char *)buf + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

/* Network byte-order conversions for struct pg_tcp_qp_info */
static void pg_tcp_qp_info_to_net(const struct pg_tcp_qp_info *host, struct pg_tcp_qp_info *net) {
    net->qpn = htonl(host->qpn);
    net->psn = htonl(host->psn);
    net->lid = htons(host->lid);
    net->reserved = htons(host->reserved);
}

static void pg_tcp_qp_info_to_host(const struct pg_tcp_qp_info *net, struct pg_tcp_qp_info *host) {
    host->qpn = ntohl(net->qpn);
    host->psn = ntohl(net->psn);
    host->lid = ntohs(net->lid);
    host->reserved = ntohs(net->reserved);
}

/* Set send/receive timeouts on an active socket */
static int pg_tcp_set_sock_timeouts(int fd, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }
    return 0;
}

/* Create, bind, and listen on a TCP server socket with SO_REUSEADDR */
static int pg_tcp_create_listener(int port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL, *t = NULL;
    char port_str[16];
    int sockfd = -1;
    int opt = 1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(NULL, port_str, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "[pg_tcp] Error getaddrinfo for port %d: %s\n", port, gai_strerror(rc));
        return -1;
    }

    for (t = res; t != NULL; t = t->ai_next) {
        sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sockfd < 0) continue;

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            close(sockfd);
            sockfd = -1;
            continue;
        }

        if (bind(sockfd, t->ai_addr, t->ai_addrlen) == 0) {
            break;
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd < 0) {
        fprintf(stderr, "[pg_tcp] Error: Could not bind listener to port %d\n", port);
        return -1;
    }

    if (listen(sockfd, 16) < 0) {
        perror("[pg_tcp] Error: listen failed");
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* Connect to target host:port with retry loop and deadline */
static int pg_tcp_connect_retry(const char *hostname, int port, int timeout_sec, int retry_ms) {
    struct addrinfo hints;
    struct addrinfo *res = NULL, *t = NULL;
    char port_str[16];
    struct timespec start, now;
    int sockfd = -1;

    snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    clock_gettime(CLOCK_MONOTONIC, &start);

    while (1) {
        int rc = getaddrinfo(hostname, port_str, &hints, &res);
        if (rc == 0) {
            for (t = res; t != NULL; t = t->ai_next) {
                sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
                if (sockfd < 0) continue;

                if (pg_tcp_set_sock_timeouts(sockfd, PG_TCP_SOCK_TIMEOUT_SEC) < 0) {
                    close(sockfd);
                    sockfd = -1;
                    continue;
                }

                if (connect(sockfd, t->ai_addr, t->ai_addrlen) == 0) {
                    /* Connected successfully */
                    break;
                }

                close(sockfd);
                sockfd = -1;
            }
            freeaddrinfo(res);
            res = NULL;
        }

        if (sockfd >= 0) {
            return sockfd;
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)timeout_sec) {
            fprintf(stderr, "[pg_tcp] Error: Connection to %s:%d timed out after %.1f seconds\n",
                    hostname, port, elapsed);
            return -1;
        }

        struct timespec ts;
        ts.tv_sec = retry_ms / 1000;
        ts.tv_nsec = (retry_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
}

/* Accept incoming connection on listener with deadline */
static int pg_tcp_accept_timeout(int listener_fd, int timeout_sec) {
    struct pollfd pfd;
    pfd.fd = listener_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int prc = poll(&pfd, 1, timeout_sec * 1000);
    if (prc <= 0) {
        if (prc == 0) {
            fprintf(stderr, "[pg_tcp] Error: Accept timed out after %d seconds\n", timeout_sec);
        } else {
            perror("[pg_tcp] Error: poll on listener failed");
        }
        return -1;
    }

    int connfd = accept(listener_fd, NULL, NULL);
    if (connfd < 0) {
        perror("[pg_tcp] Error: accept failed");
        return -1;
    }

    if (pg_tcp_set_sock_timeouts(connfd, PG_TCP_SOCK_TIMEOUT_SEC) < 0) {
        perror("[pg_tcp] Warning: set socket timeouts failed on accepted connection");
    }

    return connfd;
}

/* Cleanup InfiniBand resources in reverse order */
void pg_rdma_cleanup(struct pg_context *ctx) {
    if (!ctx) return;

    /* Deregister all application MRs in the lazy cache (ADR-0002) */
    for (int i = 0; i < ctx->mr_cache_count; i++) {
        if (ctx->mr_cache[i].mr) {
            ibv_dereg_mr(ctx->mr_cache[i].mr);
            ctx->mr_cache[i].mr = NULL;
        }
    }
    ctx->mr_cache_count = 0;

    /* Clean up internal staging and working buffers */
    if (ctx->staging_mr) {
        ibv_dereg_mr(ctx->staging_mr);
        ctx->staging_mr = NULL;
    }
    if (ctx->staging_buf) {
        free(ctx->staging_buf);
        ctx->staging_buf = NULL;
    }
    ctx->staging_capacity = 0;

    if (ctx->work_mr) {
        ibv_dereg_mr(ctx->work_mr);
        ctx->work_mr = NULL;
    }
    if (ctx->work_buf) {
        free(ctx->work_buf);
        ctx->work_buf = NULL;
    }
    ctx->work_capacity = 0;

    if (ctx->qp_to_next) {
        ibv_destroy_qp(ctx->qp_to_next);
        ctx->qp_to_next = NULL;
    }
    if (ctx->qp_from_prev) {
        ibv_destroy_qp(ctx->qp_from_prev);
        ctx->qp_from_prev = NULL;
    }
    if (ctx->cq) {
        ibv_destroy_cq(ctx->cq);
        ctx->cq = NULL;
    }
    for (int dir = 0; dir < 2; dir++) {
        if (ctx->recv_slot_mr[dir]) {
            ibv_dereg_mr(ctx->recv_slot_mr[dir]);
            ctx->recv_slot_mr[dir] = NULL;
        }
        if (ctx->recv_slot_raw_mem[dir]) {
            free(ctx->recv_slot_raw_mem[dir]);
            ctx->recv_slot_raw_mem[dir] = NULL;
        }
        if (ctx->eager_send_hdr_mr[dir]) {
            ibv_dereg_mr(ctx->eager_send_hdr_mr[dir]);
            ctx->eager_send_hdr_mr[dir] = NULL;
        }
        if (ctx->ctrl_send_mr[dir]) {
            ibv_dereg_mr(ctx->ctrl_send_mr[dir]);
            ctx->ctrl_send_mr[dir] = NULL;
        }
    }
    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = NULL;
    }
    if (ctx->ib_ctx) {
        ibv_close_device(ctx->ib_ctx);
        ctx->ib_ctx = NULL;
    }
}

/* ========================================================================= */
/* === MODULE 2: VERBS HARDWARE & QP LIFECYCLE                          === */
/* ========================================================================= */

/* Open first IB device, query capabilities, allocate PD, CQ, QPs, and pre-post receive pool */
int pg_rdma_init_resources(struct pg_context *ctx) {
    if (!ctx) return PG_ERR_INVAL;

    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list || !dev_list[0]) {
        fprintf(stderr, "[pg_rdma] Error: No InfiniBand devices found\n");
        if (dev_list) ibv_free_device_list(dev_list);
        return PG_ERR_RDMA;
    }

    ctx->ib_ctx = ibv_open_device(dev_list[0]);
    ibv_free_device_list(dev_list);
    if (!ctx->ib_ctx) {
        fprintf(stderr, "[pg_rdma] Error: Could not open InfiniBand device\n");
        return PG_ERR_RDMA;
    }

    /* Query port attributes for LID and active MTU */
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx->ib_ctx, PG_IB_PORT, &port_attr)) {
        fprintf(stderr, "[pg_rdma] Error: Could not query IB port %d\n", PG_IB_PORT);
        pg_rdma_cleanup(ctx);
        return PG_ERR_RDMA;
    }
    ctx->local_lid = port_attr.lid;
    ctx->active_mtu = port_attr.active_mtu;

    /* Query device attributes for max_qp_wr */
    struct ibv_device_attr dev_attr;
    if (ibv_query_device(ctx->ib_ctx, &dev_attr)) {
        fprintf(stderr, "[pg_rdma] Error: Could not query device attributes\n");
        pg_rdma_cleanup(ctx);
        return PG_ERR_RDMA;
    }

    uint32_t max_send_wr = 256;
    uint32_t max_recv_wr = PG_CTRL_POOL_DEPTH;
    if (max_send_wr > (uint32_t)dev_attr.max_qp_wr) max_send_wr = dev_attr.max_qp_wr;
    if (max_recv_wr > (uint32_t)dev_attr.max_qp_wr) max_recv_wr = dev_attr.max_qp_wr;

    /* Allocate Protection Domain */
    ctx->pd = ibv_alloc_pd(ctx->ib_ctx);
    if (!ctx->pd) {
        fprintf(stderr, "[pg_rdma] Error: Could not allocate Protection Domain\n");
        pg_rdma_cleanup(ctx);
        return PG_ERR_RDMA;
    }

    /* Register unified receive and send buffers */
    for (int dir = 0; dir < 2; dir++) {
        size_t total_recv_bytes = (size_t)PG_CTRL_POOL_DEPTH * (size_t)PG_EAGER_SLOT_SIZE;
        ctx->recv_slot_raw_mem[dir] = malloc(total_recv_bytes);
        if (!ctx->recv_slot_raw_mem[dir]) {
            fprintf(stderr, "[pg_rdma] Error: Could not allocate unified receive memory for dir %d\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_NOMEM;
        }
        for (int slot = 0; slot < PG_CTRL_POOL_DEPTH; slot++) {
            ctx->recv_slot_buf[dir][slot] = (char *)ctx->recv_slot_raw_mem[dir] + (size_t)slot * (size_t)PG_EAGER_SLOT_SIZE;
        }
        ctx->recv_slot_mr[dir] = ibv_reg_mr(ctx->pd, ctx->recv_slot_raw_mem[dir], total_recv_bytes,
                                            IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->recv_slot_mr[dir]) {
            fprintf(stderr, "[pg_rdma] Error: Could not register unified receive MR for dir %d\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }

        ctx->ctrl_send_mr[dir] = ibv_reg_mr(ctx->pd, ctx->ctrl_send_buf[dir],
                                           sizeof(ctx->ctrl_send_buf[dir]),
                                           IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->ctrl_send_mr[dir]) {
            fprintf(stderr, "[pg_rdma] Error: Could not register control send MR for dir %d\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }

        /* Eager header send MR */
        ctx->eager_send_hdr_mr[dir] = ibv_reg_mr(ctx->pd, ctx->eager_send_hdr_buf[dir],
                                                 sizeof(ctx->eager_send_hdr_buf[dir]),
                                                 IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->eager_send_hdr_mr[dir]) {
            fprintf(stderr, "[pg_rdma] Error: Could not register eager header send MR for dir %d\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }
    }

    /* Create shared CQ for both QPs */
    int cq_depth = (int)((max_send_wr + max_recv_wr) * 2);
    ctx->cq = ibv_create_cq(ctx->ib_ctx, cq_depth, NULL, NULL, 0);
    if (!ctx->cq) {
        fprintf(stderr, "[pg_rdma] Error: Could not create shared CQ of depth %d\n", cq_depth);
        pg_rdma_cleanup(ctx);
        return PG_ERR_RDMA;
    }

    /* Create qp_to_next (dir 0) and qp_from_prev (dir 1) with inline stepping */
    for (int dir = 0; dir < 2; dir++) {
        struct ibv_qp **qp_ptr = (dir == PG_QP_DIR_TO_NEXT) ? &ctx->qp_to_next : &ctx->qp_from_prev;
        int try_inline;

        for (try_inline = PG_MAX_INLINE_DECLARE;; try_inline -= 64) {
            struct ibv_qp_init_attr init_attr = {
                .send_cq = ctx->cq,
                .recv_cq = ctx->cq,
                .cap = {
                    .max_send_wr = max_send_wr,
                    .max_recv_wr = max_recv_wr,
                    .max_send_sge = 2,
                    .max_recv_sge = 1,
                    .max_inline_data = (uint32_t)(try_inline > 0 ? try_inline : 0)
                },
                .qp_type = IBV_QPT_RC
            };

            *qp_ptr = ibv_create_qp(ctx->pd, &init_attr);
            if (*qp_ptr || try_inline <= 0) break;
        }

        if (!*qp_ptr) {
            fprintf(stderr, "[pg_rdma] Error: Could not create RC QP for dir %d: %s\n", dir, strerror(errno));
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }

        /* Read back actual runtime max_inline_data and sq_depth */
        struct ibv_qp_attr qp_attr;
        struct ibv_qp_init_attr qp_init_attr;
        if (ibv_query_qp(*qp_ptr, &qp_attr, IBV_QP_CAP, &qp_init_attr)) {
            fprintf(stderr, "[pg_rdma] Error: Could not query QP attributes for dir %d\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }
        ctx->max_inline_data[dir] = qp_init_attr.cap.max_inline_data;
        ctx->sq_depth[dir] = qp_init_attr.cap.max_send_wr;

        /* Transition QP to INIT */
        struct ibv_qp_attr attr = {
            .qp_state = IBV_QPS_INIT,
            .pkey_index = 0,
            .port_num = PG_IB_PORT,
            .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
        };
        if (ibv_modify_qp(*qp_ptr, &attr,
                IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
            fprintf(stderr, "[pg_rdma] Error: Failed to transition QP %d to INIT\n", dir);
            pg_rdma_cleanup(ctx);
            return PG_ERR_RDMA;
        }

        /* Pre-post 32 unified 2-SGE receive WRs (header + eager payload buffer) */
        for (int slot = 0; slot < PG_CTRL_POOL_DEPTH; slot++) {
            if (pg_repost_recv_slot(ctx, dir, slot)) {
                fprintf(stderr, "[pg_rdma] Error: Failed to pre-post recv slot %d on QP %d\n", slot, dir);
                pg_rdma_cleanup(ctx);
                return PG_ERR_RDMA;
            }
        }
    }

    /* Populate local QP metadata for TCP bootstrap */
    srand48((long)(time(NULL) ^ (ctx->rank << 16)));
    ctx->local_to_next.qpn = ctx->qp_to_next->qp_num;
    ctx->local_to_next.psn = (uint32_t)(lrand48() & 0xFFFFFF);
    ctx->local_to_next.lid = ctx->local_lid;
    ctx->local_to_next.reserved = 0;

    ctx->local_from_prev.qpn = ctx->qp_from_prev->qp_num;
    ctx->local_from_prev.psn = (uint32_t)(lrand48() & 0xFFFFFF);
    ctx->local_from_prev.lid = ctx->local_lid;
    ctx->local_from_prev.reserved = 0;

    return PG_SUCCESS;
}

/* Post one 2-SGE eager payload SEND message (ADR-0002) */
int pg_post_eager_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *hdr,
                       void *payload_addr, uint32_t payload_len, uint32_t lkey, int signaled, int slot) {
    if (!ctx || !hdr || (qp_dir != PG_QP_DIR_TO_NEXT && qp_dir != PG_QP_DIR_FROM_PREV)) {
        return PG_ERR_INVAL;
    }

    struct ibv_qp *target_qp = (qp_dir == PG_QP_DIR_TO_NEXT) ? ctx->qp_to_next : ctx->qp_from_prev;
    int s = slot % PG_CTRL_POOL_DEPTH;

    /* Copy header into dedicated slot in registered eager_send_hdr_buf */
    memcpy(ctx->eager_send_hdr_buf[qp_dir][s], hdr, sizeof(*hdr));

    struct ibv_sge sges[2];
    sges[0].addr   = (uintptr_t)ctx->eager_send_hdr_buf[qp_dir][s];
    sges[0].length = sizeof(*hdr);
    sges[0].lkey   = ctx->eager_send_hdr_mr[qp_dir]->lkey;

    sges[1].addr   = (uintptr_t)payload_addr;
    sges[1].length = payload_len;
    sges[1].lkey   = lkey;

    struct ibv_send_wr wr = {
        .wr_id      = pg_make_wr_slot(qp_dir, PG_WR_TYPE_EAGER_SEND, s),
        .opcode     = IBV_WR_SEND,
        .send_flags = signaled ? IBV_SEND_SIGNALED : 0,
        .sg_list    = sges,
        .num_sge    = (payload_len > 0) ? 2 : 1,
        .next       = NULL
    };

    if ((sizeof(*hdr) + (size_t)payload_len) <= ctx->max_inline_data[qp_dir]) {
        wr.send_flags |= IBV_SEND_INLINE;
    }

    struct ibv_send_wr *bad_wr = NULL;
    if (ibv_post_send(target_qp, &wr, &bad_wr)) {
        perror("[pg_rdma] Error: ibv_post_send failed for eager SEND");
        return PG_ERR_RDMA;
    }

    return PG_SUCCESS;
}

/* Transition QP to RTR then RTS using remote connection metadata */
int pg_rdma_connect_qp(struct ibv_qp *qp, const struct pg_tcp_qp_info *remote,
                       uint32_t my_psn, enum ibv_mtu active_mtu) {
    if (!qp || !remote) return PG_ERR_INVAL;

    /* 1. Transition to RTR */
    struct ibv_qp_attr attr = {
        .qp_state           = IBV_QPS_RTR,
        .path_mtu           = active_mtu,
        .dest_qp_num        = remote->qpn,
        .rq_psn             = remote->psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer      = 12,
        .ah_attr            = {
            .is_global      = 0,
            .dlid           = remote->lid,
            .sl             = 0,
            .src_path_bits  = 0,
            .port_num       = PG_IB_PORT
        }
    };

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "[pg_rdma] Error: Failed to modify QP to RTR (dest_qpn=%u, dlid=%u)\n",
                remote->qpn, remote->lid);
        return PG_ERR_RDMA;
    }

    /* 2. Transition to RTS */
    memset(&attr, 0, sizeof(attr));
    attr.qp_state       = IBV_QPS_RTS;
    attr.timeout        = 14;
    attr.retry_cnt      = 7;
    attr.rnr_retry      = 7;
    attr.sq_psn         = my_psn;
    attr.max_rd_atomic  = 1;

    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(qp, &attr, flags)) {
        fprintf(stderr, "[pg_rdma] Error: Failed to modify QP to RTS\n");
        return PG_ERR_RDMA;
    }

    return PG_SUCCESS;
}

/* Post one signaled control SEND message */
int pg_post_ctrl_send(struct pg_context *ctx, int qp_dir, const struct pg_ctrl_msg *msg) {
    if (!ctx || !msg || (qp_dir != PG_QP_DIR_TO_NEXT && qp_dir != PG_QP_DIR_FROM_PREV)) {
        return PG_ERR_INVAL;
    }

    struct ibv_qp *target_qp = (qp_dir == PG_QP_DIR_TO_NEXT) ? ctx->qp_to_next : ctx->qp_from_prev;
    struct ibv_sge sge = {
        .addr   = (uintptr_t)msg,
        .length = sizeof(*msg),
        .lkey   = 0
    };
    struct ibv_send_wr wr = {
        .wr_id      = pg_make_wr(qp_dir, PG_WR_TYPE_SEND_CTRL),
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list    = &sge,
        .num_sge    = 1,
        .next       = NULL
    };
    struct ibv_send_wr *bad_wr = NULL;

    if (ctx->max_inline_data[qp_dir] >= (uint32_t)sizeof(*msg)) {
        wr.send_flags |= IBV_SEND_INLINE;
    } else {
        memcpy(ctx->ctrl_send_buf[qp_dir], msg, sizeof(*msg));
        sge.addr = (uintptr_t)ctx->ctrl_send_buf[qp_dir];
        sge.lkey = ctx->ctrl_send_mr[qp_dir]->lkey;
    }

    if (ibv_post_send(target_qp, &wr, &bad_wr)) {
        perror("[pg_rdma] Error: ibv_post_send failed for control SEND");
        return PG_ERR_RDMA;
    }

    return PG_SUCCESS;
}

/* ========================================================================= */
/* === MODULE 3: MEMORY REGISTRATION & STAGING CACHE                    === */
/* ========================================================================= */

/* Lazy MR Cache registration helper (ADR-0002) */
struct ibv_mr *pg_get_or_reg_mr(struct pg_context *ctx, void *addr, size_t length, int access_flags) {
    if (!ctx || !addr || length == 0) return NULL;

    /* Check if exact (addr, length) or containing MR with sufficient access_flags is already cached */
    for (int i = 0; i < ctx->mr_cache_count; i++) {
        uintptr_t cached_start = (uintptr_t)ctx->mr_cache[i].addr;
        uintptr_t cached_end = cached_start + ctx->mr_cache[i].length;
        uintptr_t req_start = (uintptr_t)addr;
        uintptr_t req_end = req_start + length;

        if (req_start >= cached_start && req_end <= cached_end) {
            if ((ctx->mr_cache[i].access_flags & access_flags) == access_flags) {
                return ctx->mr_cache[i].mr;
            }
        }
    }

    if (ctx->mr_cache_count >= PG_MR_CACHE_MAX) {
        fprintf(stderr, "[pg_rdma] Error: MR cache full (max %d entries)\n", PG_MR_CACHE_MAX);
        return NULL;
    }

    struct ibv_mr *mr = ibv_reg_mr(ctx->pd, addr, length, access_flags);
    if (!mr) {
        perror("[pg_rdma] Error: ibv_reg_mr failed");
        return NULL;
    }

    int idx = ctx->mr_cache_count++;
    ctx->mr_cache[idx].addr = addr;
    ctx->mr_cache[idx].length = length;
    ctx->mr_cache[idx].access_flags = access_flags;
    ctx->mr_cache[idx].mr = mr;

    return mr;
}

/* Ensure internal staging and safe-mode work buffers are allocated and registered (ADR-0002) */
int pg_ensure_internal_buffers(struct pg_context *ctx, size_t count_bytes, size_t segment_bytes) {
    if (!ctx) return PG_ERR_INVAL;

    /* 1. Staging buffer: needs at least segment_bytes */
    if (ctx->staging_capacity < segment_bytes) {
        if (ctx->staging_mr) {
            ibv_dereg_mr(ctx->staging_mr);
            ctx->staging_mr = NULL;
        }
        if (ctx->staging_buf) {
            free(ctx->staging_buf);
            ctx->staging_buf = NULL;
        }

        if (posix_memalign((void **)&ctx->staging_buf, 64, segment_bytes) != 0 || !ctx->staging_buf) {
            ctx->staging_capacity = 0;
            return PG_ERR_NOMEM;
        }
        ctx->staging_capacity = segment_bytes;

        ctx->staging_mr = ibv_reg_mr(ctx->pd, ctx->staging_buf, ctx->staging_capacity,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!ctx->staging_mr) {
            perror("[pg_rdma] Error: ibv_reg_mr failed for staging buffer");
            free(ctx->staging_buf);
            ctx->staging_buf = NULL;
            ctx->staging_capacity = 0;
            return PG_ERR_RDMA;
        }
    }

#ifndef PG_WORKBUFFER_INPLACE
    /* 2. Work buffer: needs at least count_bytes in safe mode */
    if (ctx->work_capacity < count_bytes) {
        if (ctx->work_mr) {
            ibv_dereg_mr(ctx->work_mr);
            ctx->work_mr = NULL;
        }
        if (ctx->work_buf) {
            free(ctx->work_buf);
            ctx->work_buf = NULL;
        }

        if (posix_memalign((void **)&ctx->work_buf, 64, count_bytes) != 0 || !ctx->work_buf) {
            ctx->work_capacity = 0;
            return PG_ERR_NOMEM;
        }
        ctx->work_capacity = count_bytes;

        ctx->work_mr = ibv_reg_mr(ctx->pd, ctx->work_buf, ctx->work_capacity,
                                  IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!ctx->work_mr) {
            perror("[pg_rdma] Error: ibv_reg_mr failed for work buffer");
            free(ctx->work_buf);
            ctx->work_buf = NULL;
            ctx->work_capacity = 0;
            return PG_ERR_RDMA;
        }
    }
#else
    (void)count_bytes;
#endif

    return PG_SUCCESS;
}

/* Initialize Runtime Hyperparameters from Environment with compile-time defaults */
void pg_init_tuning_params(struct pg_context *ctx) {
    if (!ctx) return;

    ctx->pipeline_chunk = PG_PIPELINE_CHUNK;
    const char *chunk_env = getenv("PG_PIPELINE_CHUNK");
    if (chunk_env && *chunk_env) {
        size_t val = (size_t)strtoul(chunk_env, NULL, 10);
        if (val > 0) ctx->pipeline_chunk = val;
    }

    ctx->rdma_window = PG_RDMA_WINDOW;
    const char *win_env = getenv("PG_RDMA_WINDOW");
    if (win_env && *win_env) {
        int val = atoi(win_env);
        if (val > 0) ctx->rdma_window = val;
    }

    ctx->rdma_signal_interval = PG_RDMA_SIGNAL_INTERVAL;
    const char *sig_env = getenv("PG_RDMA_SIGNAL_INTERVAL");
    if (sig_env && *sig_env) {
        int val = atoi(sig_env);
        if (val > 0) ctx->rdma_signal_interval = val;
    }

    ctx->batch_size = PG_DEFAULT_BATCH_SIZE;
    const char *batch_env = getenv("PG_BATCH_SIZE");
    if (batch_env && *batch_env) {
        int val = atoi(batch_env);
        if (val > 0) ctx->batch_size = val;
    }

    ctx->eager_threshold = PG_EAGER_THRESHOLD;
    const char *eager_env = getenv("PG_EAGER_THRESHOLD");
    if (eager_env && *eager_env) {
        size_t val = (size_t)strtoul(eager_env, NULL, 10);
        if (val > 0) ctx->eager_threshold = val;
    }

    ctx->eager_window = PG_EAGER_WINDOW;
    const char *ewin_env = getenv("PG_EAGER_WINDOW");
    if (ewin_env && *ewin_env) {
        int val = atoi(ewin_env);
        if (val > 0) ctx->eager_window = val;
    }

    ctx->use_streaming_stores = 1;
    const char *stream_env = getenv("PG_USE_STREAMING_STORES");
    if (stream_env && *stream_env) {
        ctx->use_streaming_stores = atoi(stream_env);
    }
}

/* ========================================================================= */
/* === MODULE 5: SSE4.2 VECTOR REDUCTION COMPUTE KERNELS                === */
/* ========================================================================= */

/* Vectorized CPU reduction engine with SSE4.2 and 4x loop unrolling */
void pg_reduce_buffer(void *dest, const void *src, int count,
                      DATATYPE datatype, OPERATION op, int use_streaming) {
    (void)use_streaming;
    if (count <= 0 || !dest || !src) return;

    switch (datatype) {
        case PG_INT: {
            int32_t *d = (int32_t *)dest;
            const int32_t *s = (const int32_t *)src;
            int i = 0;

            if (op == PG_SUM) {
                for (; i + 16 <= count; i += 16) {
                    __m128i vd0 = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vd1 = _mm_loadu_si128((const __m128i *)(d + i + 4));
                    __m128i vd2 = _mm_loadu_si128((const __m128i *)(d + i + 8));
                    __m128i vd3 = _mm_loadu_si128((const __m128i *)(d + i + 12));
                    __m128i vs0 = _mm_loadu_si128((const __m128i *)(s + i));
                    __m128i vs1 = _mm_loadu_si128((const __m128i *)(s + i + 4));
                    __m128i vs2 = _mm_loadu_si128((const __m128i *)(s + i + 8));
                    __m128i vs3 = _mm_loadu_si128((const __m128i *)(s + i + 12));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_add_epi32(vd0, vs0));
                    _mm_storeu_si128((__m128i *)(d + i + 4), _mm_add_epi32(vd1, vs1));
                    _mm_storeu_si128((__m128i *)(d + i + 8), _mm_add_epi32(vd2, vs2));
                    _mm_storeu_si128((__m128i *)(d + i + 12), _mm_add_epi32(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128i vd = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vs = _mm_loadu_si128((const __m128i *)(s + i));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_add_epi32(vd, vs));
                }
                for (; i < count; i++) d[i] += s[i];
            } else if (op == PG_MIN) {
                for (; i + 16 <= count; i += 16) {
                    __m128i vd0 = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vd1 = _mm_loadu_si128((const __m128i *)(d + i + 4));
                    __m128i vd2 = _mm_loadu_si128((const __m128i *)(d + i + 8));
                    __m128i vd3 = _mm_loadu_si128((const __m128i *)(d + i + 12));
                    __m128i vs0 = _mm_loadu_si128((const __m128i *)(s + i));
                    __m128i vs1 = _mm_loadu_si128((const __m128i *)(s + i + 4));
                    __m128i vs2 = _mm_loadu_si128((const __m128i *)(s + i + 8));
                    __m128i vs3 = _mm_loadu_si128((const __m128i *)(s + i + 12));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_min_epi32(vd0, vs0));
                    _mm_storeu_si128((__m128i *)(d + i + 4), _mm_min_epi32(vd1, vs1));
                    _mm_storeu_si128((__m128i *)(d + i + 8), _mm_min_epi32(vd2, vs2));
                    _mm_storeu_si128((__m128i *)(d + i + 12), _mm_min_epi32(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128i vd = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vs = _mm_loadu_si128((const __m128i *)(s + i));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_min_epi32(vd, vs));
                }
                for (; i < count; i++) { if (s[i] < d[i]) d[i] = s[i]; }
            } else if (op == PG_MAX) {
                for (; i + 16 <= count; i += 16) {
                    __m128i vd0 = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vd1 = _mm_loadu_si128((const __m128i *)(d + i + 4));
                    __m128i vd2 = _mm_loadu_si128((const __m128i *)(d + i + 8));
                    __m128i vd3 = _mm_loadu_si128((const __m128i *)(d + i + 12));
                    __m128i vs0 = _mm_loadu_si128((const __m128i *)(s + i));
                    __m128i vs1 = _mm_loadu_si128((const __m128i *)(s + i + 4));
                    __m128i vs2 = _mm_loadu_si128((const __m128i *)(s + i + 8));
                    __m128i vs3 = _mm_loadu_si128((const __m128i *)(s + i + 12));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_max_epi32(vd0, vs0));
                    _mm_storeu_si128((__m128i *)(d + i + 4), _mm_max_epi32(vd1, vs1));
                    _mm_storeu_si128((__m128i *)(d + i + 8), _mm_max_epi32(vd2, vs2));
                    _mm_storeu_si128((__m128i *)(d + i + 12), _mm_max_epi32(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128i vd = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vs = _mm_loadu_si128((const __m128i *)(s + i));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_max_epi32(vd, vs));
                }
                for (; i < count; i++) { if (s[i] > d[i]) d[i] = s[i]; }
            } else if (op == PG_PROD) {
                for (; i + 16 <= count; i += 16) {
                    __m128i vd0 = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vd1 = _mm_loadu_si128((const __m128i *)(d + i + 4));
                    __m128i vd2 = _mm_loadu_si128((const __m128i *)(d + i + 8));
                    __m128i vd3 = _mm_loadu_si128((const __m128i *)(d + i + 12));
                    __m128i vs0 = _mm_loadu_si128((const __m128i *)(s + i));
                    __m128i vs1 = _mm_loadu_si128((const __m128i *)(s + i + 4));
                    __m128i vs2 = _mm_loadu_si128((const __m128i *)(s + i + 8));
                    __m128i vs3 = _mm_loadu_si128((const __m128i *)(s + i + 12));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_mullo_epi32(vd0, vs0));
                    _mm_storeu_si128((__m128i *)(d + i + 4), _mm_mullo_epi32(vd1, vs1));
                    _mm_storeu_si128((__m128i *)(d + i + 8), _mm_mullo_epi32(vd2, vs2));
                    _mm_storeu_si128((__m128i *)(d + i + 12), _mm_mullo_epi32(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128i vd = _mm_loadu_si128((const __m128i *)(d + i));
                    __m128i vs = _mm_loadu_si128((const __m128i *)(s + i));
                    _mm_storeu_si128((__m128i *)(d + i), _mm_mullo_epi32(vd, vs));
                }
                for (; i < count; i++) d[i] *= s[i];
            }
            break;
        }

        case PG_FLOAT: {
            float *d = (float *)dest;
            const float *s = (const float *)src;
            int i = 0;

            if (op == PG_SUM) {
                for (; i + 16 <= count; i += 16) {
                    __m128 vd0 = _mm_loadu_ps(d + i);
                    __m128 vd1 = _mm_loadu_ps(d + i + 4);
                    __m128 vd2 = _mm_loadu_ps(d + i + 8);
                    __m128 vd3 = _mm_loadu_ps(d + i + 12);
                    __m128 vs0 = _mm_loadu_ps(s + i);
                    __m128 vs1 = _mm_loadu_ps(s + i + 4);
                    __m128 vs2 = _mm_loadu_ps(s + i + 8);
                    __m128 vs3 = _mm_loadu_ps(s + i + 12);
                    _mm_storeu_ps(d + i, _mm_add_ps(vd0, vs0));
                    _mm_storeu_ps(d + i + 4, _mm_add_ps(vd1, vs1));
                    _mm_storeu_ps(d + i + 8, _mm_add_ps(vd2, vs2));
                    _mm_storeu_ps(d + i + 12, _mm_add_ps(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128 vd = _mm_loadu_ps(d + i);
                    __m128 vs = _mm_loadu_ps(s + i);
                    _mm_storeu_ps(d + i, _mm_add_ps(vd, vs));
                }
                for (; i < count; i++) d[i] += s[i];
            } else if (op == PG_MIN) {
                for (; i + 16 <= count; i += 16) {
                    __m128 vd0 = _mm_loadu_ps(d + i);
                    __m128 vd1 = _mm_loadu_ps(d + i + 4);
                    __m128 vd2 = _mm_loadu_ps(d + i + 8);
                    __m128 vd3 = _mm_loadu_ps(d + i + 12);
                    __m128 vs0 = _mm_loadu_ps(s + i);
                    __m128 vs1 = _mm_loadu_ps(s + i + 4);
                    __m128 vs2 = _mm_loadu_ps(s + i + 8);
                    __m128 vs3 = _mm_loadu_ps(s + i + 12);
                    _mm_storeu_ps(d + i, _mm_min_ps(vd0, vs0));
                    _mm_storeu_ps(d + i + 4, _mm_min_ps(vd1, vs1));
                    _mm_storeu_ps(d + i + 8, _mm_min_ps(vd2, vs2));
                    _mm_storeu_ps(d + i + 12, _mm_min_ps(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128 vd = _mm_loadu_ps(d + i);
                    __m128 vs = _mm_loadu_ps(s + i);
                    _mm_storeu_ps(d + i, _mm_min_ps(vd, vs));
                }
                for (; i < count; i++) { if (s[i] < d[i]) d[i] = s[i]; }
            } else if (op == PG_MAX) {
                for (; i + 16 <= count; i += 16) {
                    __m128 vd0 = _mm_loadu_ps(d + i);
                    __m128 vd1 = _mm_loadu_ps(d + i + 4);
                    __m128 vd2 = _mm_loadu_ps(d + i + 8);
                    __m128 vd3 = _mm_loadu_ps(d + i + 12);
                    __m128 vs0 = _mm_loadu_ps(s + i);
                    __m128 vs1 = _mm_loadu_ps(s + i + 4);
                    __m128 vs2 = _mm_loadu_ps(s + i + 8);
                    __m128 vs3 = _mm_loadu_ps(s + i + 12);
                    _mm_storeu_ps(d + i, _mm_max_ps(vd0, vs0));
                    _mm_storeu_ps(d + i + 4, _mm_max_ps(vd1, vs1));
                    _mm_storeu_ps(d + i + 8, _mm_max_ps(vd2, vs2));
                    _mm_storeu_ps(d + i + 12, _mm_max_ps(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128 vd = _mm_loadu_ps(d + i);
                    __m128 vs = _mm_loadu_ps(s + i);
                    _mm_storeu_ps(d + i, _mm_max_ps(vd, vs));
                }
                for (; i < count; i++) { if (s[i] > d[i]) d[i] = s[i]; }
            } else if (op == PG_PROD) {
                for (; i + 16 <= count; i += 16) {
                    __m128 vd0 = _mm_loadu_ps(d + i);
                    __m128 vd1 = _mm_loadu_ps(d + i + 4);
                    __m128 vd2 = _mm_loadu_ps(d + i + 8);
                    __m128 vd3 = _mm_loadu_ps(d + i + 12);
                    __m128 vs0 = _mm_loadu_ps(s + i);
                    __m128 vs1 = _mm_loadu_ps(s + i + 4);
                    __m128 vs2 = _mm_loadu_ps(s + i + 8);
                    __m128 vs3 = _mm_loadu_ps(s + i + 12);
                    _mm_storeu_ps(d + i, _mm_mul_ps(vd0, vs0));
                    _mm_storeu_ps(d + i + 4, _mm_mul_ps(vd1, vs1));
                    _mm_storeu_ps(d + i + 8, _mm_mul_ps(vd2, vs2));
                    _mm_storeu_ps(d + i + 12, _mm_mul_ps(vd3, vs3));
                }
                for (; i + 4 <= count; i += 4) {
                    __m128 vd = _mm_loadu_ps(d + i);
                    __m128 vs = _mm_loadu_ps(s + i);
                    _mm_storeu_ps(d + i, _mm_mul_ps(vd, vs));
                }
                for (; i < count; i++) d[i] *= s[i];
            }
            break;
        }

        case PG_DOUBLE: {
            double *d = (double *)dest;
            const double *s = (const double *)src;
            int i = 0;

            if (op == PG_SUM) {
                for (; i + 8 <= count; i += 8) {
                    __m128d vd0 = _mm_loadu_pd(d + i);
                    __m128d vd1 = _mm_loadu_pd(d + i + 2);
                    __m128d vd2 = _mm_loadu_pd(d + i + 4);
                    __m128d vd3 = _mm_loadu_pd(d + i + 6);
                    __m128d vs0 = _mm_loadu_pd(s + i);
                    __m128d vs1 = _mm_loadu_pd(s + i + 2);
                    __m128d vs2 = _mm_loadu_pd(s + i + 4);
                    __m128d vs3 = _mm_loadu_pd(s + i + 6);
                    _mm_storeu_pd(d + i, _mm_add_pd(vd0, vs0));
                    _mm_storeu_pd(d + i + 2, _mm_add_pd(vd1, vs1));
                    _mm_storeu_pd(d + i + 4, _mm_add_pd(vd2, vs2));
                    _mm_storeu_pd(d + i + 6, _mm_add_pd(vd3, vs3));
                }
                for (; i + 2 <= count; i += 2) {
                    __m128d vd = _mm_loadu_pd(d + i);
                    __m128d vs = _mm_loadu_pd(s + i);
                    _mm_storeu_pd(d + i, _mm_add_pd(vd, vs));
                }
                for (; i < count; i++) d[i] += s[i];
            } else if (op == PG_MIN) {
                for (; i + 8 <= count; i += 8) {
                    __m128d vd0 = _mm_loadu_pd(d + i);
                    __m128d vd1 = _mm_loadu_pd(d + i + 2);
                    __m128d vd2 = _mm_loadu_pd(d + i + 4);
                    __m128d vd3 = _mm_loadu_pd(d + i + 6);
                    __m128d vs0 = _mm_loadu_pd(s + i);
                    __m128d vs1 = _mm_loadu_pd(s + i + 2);
                    __m128d vs2 = _mm_loadu_pd(s + i + 4);
                    __m128d vs3 = _mm_loadu_pd(s + i + 6);
                    _mm_storeu_pd(d + i, _mm_min_pd(vd0, vs0));
                    _mm_storeu_pd(d + i + 2, _mm_min_pd(vd1, vs1));
                    _mm_storeu_pd(d + i + 4, _mm_min_pd(vd2, vs2));
                    _mm_storeu_pd(d + i + 6, _mm_min_pd(vd3, vs3));
                }
                for (; i + 2 <= count; i += 2) {
                    __m128d vd = _mm_loadu_pd(d + i);
                    __m128d vs = _mm_loadu_pd(s + i);
                    _mm_storeu_pd(d + i, _mm_min_pd(vd, vs));
                }
                for (; i < count; i++) { if (s[i] < d[i]) d[i] = s[i]; }
            } else if (op == PG_MAX) {
                for (; i + 8 <= count; i += 8) {
                    __m128d vd0 = _mm_loadu_pd(d + i);
                    __m128d vd1 = _mm_loadu_pd(d + i + 2);
                    __m128d vd2 = _mm_loadu_pd(d + i + 4);
                    __m128d vd3 = _mm_loadu_pd(d + i + 6);
                    __m128d vs0 = _mm_loadu_pd(s + i);
                    __m128d vs1 = _mm_loadu_pd(s + i + 2);
                    __m128d vs2 = _mm_loadu_pd(s + i + 4);
                    __m128d vs3 = _mm_loadu_pd(s + i + 6);
                    _mm_storeu_pd(d + i, _mm_max_pd(vd0, vs0));
                    _mm_storeu_pd(d + i + 2, _mm_max_pd(vd1, vs1));
                    _mm_storeu_pd(d + i + 4, _mm_max_pd(vd2, vs2));
                    _mm_storeu_pd(d + i + 6, _mm_max_pd(vd3, vs3));
                }
                for (; i + 2 <= count; i += 2) {
                    __m128d vd = _mm_loadu_pd(d + i);
                    __m128d vs = _mm_loadu_pd(s + i);
                    _mm_storeu_pd(d + i, _mm_max_pd(vd, vs));
                }
                for (; i < count; i++) { if (s[i] > d[i]) d[i] = s[i]; }
            } else if (op == PG_PROD) {
                for (; i + 8 <= count; i += 8) {
                    __m128d vd0 = _mm_loadu_pd(d + i);
                    __m128d vd1 = _mm_loadu_pd(d + i + 2);
                    __m128d vd2 = _mm_loadu_pd(d + i + 4);
                    __m128d vd3 = _mm_loadu_pd(d + i + 6);
                    __m128d vs0 = _mm_loadu_pd(s + i);
                    __m128d vs1 = _mm_loadu_pd(s + i + 2);
                    __m128d vs2 = _mm_loadu_pd(s + i + 4);
                    __m128d vs3 = _mm_loadu_pd(s + i + 6);
                    _mm_storeu_pd(d + i, _mm_mul_pd(vd0, vs0));
                    _mm_storeu_pd(d + i + 2, _mm_mul_pd(vd1, vs1));
                    _mm_storeu_pd(d + i + 4, _mm_mul_pd(vd2, vs2));
                    _mm_storeu_pd(d + i + 6, _mm_mul_pd(vd3, vs3));
                }
                for (; i + 2 <= count; i += 2) {
                    __m128d vd = _mm_loadu_pd(d + i);
                    __m128d vs = _mm_loadu_pd(s + i);
                    _mm_storeu_pd(d + i, _mm_mul_pd(vd, vs));
                }
                for (; i < count; i++) d[i] *= s[i];
            }
            break;
        }

        default:
            break;
    }
}

/* ========================================================================= */
/* === MODULE 4: PROGRESS ENGINE & CQ DISPATCH                          === */
/* ========================================================================= */

/* Post one signaled RDMA_WRITE operation */
int pg_post_rdma_write(struct pg_context *ctx, int qp_dir, void *local_addr, size_t length,
                       uint32_t lkey, uint64_t remote_addr, uint32_t rkey) {
    if (!ctx || !local_addr || length == 0 ||
        (qp_dir != PG_QP_DIR_TO_NEXT && qp_dir != PG_QP_DIR_FROM_PREV)) {
        return PG_ERR_INVAL;
    }

    struct ibv_qp *target_qp = (qp_dir == PG_QP_DIR_TO_NEXT) ? ctx->qp_to_next : ctx->qp_from_prev;
    struct ibv_sge sge = {
        .addr   = (uintptr_t)local_addr,
        .length = (uint32_t)length,
        .lkey   = lkey
    };
    struct ibv_send_wr wr = {
        .wr_id      = pg_make_wr(qp_dir, PG_WR_TYPE_RDMA_WRITE),
        .opcode     = IBV_WR_RDMA_WRITE,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list    = &sge,
        .num_sge    = 1,
        .next       = NULL,
        .wr         = {
            .rdma = {
                .remote_addr = remote_addr,
                .rkey        = rkey
            }
        }
    };
    struct ibv_send_wr *bad_wr = NULL;

    if (ibv_post_send(target_qp, &wr, &bad_wr)) {
        perror("[pg_rdma] Error: ibv_post_send failed for RDMA_WRITE");
        return PG_ERR_RDMA;
    }

    return PG_SUCCESS;
}

/* Edge-ordered ring-only TCP bootstrap exchanging real QP metadata */
int pg_tcp_bootstrap(struct pg_context *ctx) {
    if (!ctx) return PG_ERR_INVAL;

    int listen_port = PG_TCP_BASE_PORT + ctx->rank;
    int listener_fd = pg_tcp_create_listener(listen_port);
    if (listener_fd < 0) {
        fprintf(stderr, "[pg_tcp] Rank %d failed to bind listener port %d\n", ctx->rank, listen_port);
        return PG_ERR_TCP;
    }

    int rc = PG_SUCCESS;

    /* Loop over all edges in deterministic ring order: edge e connects e -> (e+1)%size */
    for (int edge = 0; edge < ctx->size; edge++) {
        int sender_rank = edge;
        int receiver_rank = (edge + 1) % ctx->size;

        if (ctx->rank == sender_rank) {
            /* Client side for edge sender_rank -> receiver_rank */
            int target_port = PG_TCP_BASE_PORT + receiver_rank;
            const char *target_host = ctx->host_list[receiver_rank];

            int connfd = pg_tcp_connect_retry(target_host, target_port,
                                             PG_TCP_TIMEOUT_SEC, PG_TCP_RETRY_MS);
            if (connfd < 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to connect to receiver rank %d (%s:%d)\n",
                        ctx->rank, receiver_rank, target_host, target_port);
                rc = PG_ERR_TIMEOUT;
                break;
            }

            /* 1. Send local_to_next QP metadata */
            struct pg_tcp_qp_info net_info;
            pg_tcp_qp_info_to_net(&ctx->local_to_next, &net_info);
            if (pg_tcp_write_full(connfd, &net_info, sizeof(net_info)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to send QP metadata on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }

            /* 2. Read receiver's remote_to_next QP metadata */
            if (pg_tcp_read_full(connfd, &net_info, sizeof(net_info)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to read QP metadata on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }
            pg_tcp_qp_info_to_host(&net_info, &ctx->remote_to_next);

            /* Transition local qp_to_next to RTR and RTS using receiver's metadata */
            rc = pg_rdma_connect_qp(ctx->qp_to_next, &ctx->remote_to_next,
                                   ctx->local_to_next.psn, ctx->active_mtu);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "[pg_rdma] Rank %d failed to connect qp_to_next on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                break;
            }

            /* 3. Send ready tag */
            uint32_t ready_tag_net = htonl(PG_TCP_READY_TAG);
            if (pg_tcp_write_full(connfd, &ready_tag_net, sizeof(ready_tag_net)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to send ready tag on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }

            close(connfd);

        } else if (ctx->rank == receiver_rank) {
            /* Server side for edge sender_rank -> receiver_rank */
            int connfd = pg_tcp_accept_timeout(listener_fd, PG_TCP_TIMEOUT_SEC);
            if (connfd < 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to accept connection from sender rank %d\n",
                        ctx->rank, sender_rank);
                rc = PG_ERR_TIMEOUT;
                break;
            }

            /* 1. Read sender's remote_from_prev QP metadata */
            struct pg_tcp_qp_info net_info;
            if (pg_tcp_read_full(connfd, &net_info, sizeof(net_info)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to read sender QP metadata on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }
            pg_tcp_qp_info_to_host(&net_info, &ctx->remote_from_prev);

            /* Transition local qp_from_prev to RTR and RTS using sender's metadata */
            rc = pg_rdma_connect_qp(ctx->qp_from_prev, &ctx->remote_from_prev,
                                   ctx->local_from_prev.psn, ctx->active_mtu);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "[pg_rdma] Rank %d failed to connect qp_from_prev on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                break;
            }

            /* 2. Send local_from_prev QP metadata */
            pg_tcp_qp_info_to_net(&ctx->local_from_prev, &net_info);
            if (pg_tcp_write_full(connfd, &net_info, sizeof(net_info)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to send reply QP metadata on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }

            /* 3. Read and verify ready tag */
            uint32_t ready_tag_net = 0;
            if (pg_tcp_read_full(connfd, &ready_tag_net, sizeof(ready_tag_net)) != 0) {
                fprintf(stderr, "[pg_tcp] Rank %d failed to read ready tag on edge %d->%d\n",
                        ctx->rank, sender_rank, receiver_rank);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }

            uint32_t ready_tag = ntohl(ready_tag_net);
            if (ready_tag != PG_TCP_READY_TAG) {
                fprintf(stderr, "[pg_tcp] Rank %d received invalid ready tag 0x%08x (expected 0x%08x)\n",
                        ctx->rank, ready_tag, PG_TCP_READY_TAG);
                close(connfd);
                rc = PG_ERR_TCP;
                break;
            }

            close(connfd);
        }
    }

    close(listener_fd);
    return rc;
}

/* Symmetric RDMA control ring ping verification */
int pg_rdma_ring_ping(struct pg_context *ctx) {
    if (!ctx) return PG_ERR_INVAL;

    struct pg_ctrl_msg send_msg;
    memset(&send_msg, 0, sizeof(send_msg));
    send_msg.tag = PG_CTRL_TAG;
    send_msg.type = PG_CTRL_MSG_PING;
    send_msg.sender_rank = (uint16_t)ctx->rank;
    send_msg.seq = 1;

    int rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &send_msg);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg_rdma] Rank %d failed to post control ping send to next rank %d\n",
                ctx->rank, ctx->next_rank);
        return rc;
    }

    int send_done = 0;
    int recv_done = 0;

    struct pg_progress_event ev;
    if (pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, PG_CTRL_MSG_PING, (uint32_t)-1, &ev)) {
        recv_done = 1;
    }

    while (!send_done || !recv_done) {
        rc = pg_progress_wait(ctx, PG_CTRL_POLL_TIMEOUT_SEC, &ev);
        if (rc != 1) return rc < 0 ? rc : PG_ERR_TIMEOUT;

        if (ev.type == PG_WR_TYPE_SEND_CTRL && ev.qp_dir == PG_QP_DIR_TO_NEXT) {
            send_done = 1;
        } else if (ev.type == PG_WR_TYPE_RECV_CTRL && ev.qp_dir == PG_QP_DIR_FROM_PREV) {
            if (ev.msg.type == PG_CTRL_MSG_PING) {
                if (ev.msg.sender_rank != (uint16_t)ctx->prev_rank) {
                    fprintf(stderr, "[pg_rdma] Rank %d received ping from unexpected sender %u (expected %d)\n",
                            ctx->rank, ev.msg.sender_rank, ctx->prev_rank);
                    return PG_ERR_RDMA;
                }
                recv_done = 1;
            } else {
                pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
            }
        } else if (ev.type == PG_WR_TYPE_RECV_CTRL) {
            pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
        }
    }

    return PG_SUCCESS;
}

/* Pass a token once around the entire ring for Phase 1 (Collect), Phase 2 (Release), or Phase 3 (Ack) */
static int pg_barrier_token_pass(struct pg_context *ctx, uint16_t msg_type) {
    int rc;
    struct pg_progress_event ev;

    if (ctx->rank == 0) {
        struct pg_ctrl_msg msg;
        memset(&msg, 0, sizeof(msg));
        msg.tag = PG_CTRL_TAG;
        msg.type = msg_type;
        msg.sender_rank = (uint16_t)ctx->rank;

        rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &msg);
        if (rc != PG_SUCCESS) return rc;

        int send_done = 0, recv_done = 0;
        if (pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, (int)msg_type, (uint32_t)-1, &ev)) {
            recv_done = 1;
        }

        while (!send_done || !recv_done) {
            rc = pg_progress_wait(ctx, PG_CTRL_POLL_TIMEOUT_SEC, &ev);
            if (rc != 1) return rc < 0 ? rc : PG_ERR_TIMEOUT;

            if (ev.type == PG_WR_TYPE_SEND_CTRL && ev.qp_dir == PG_QP_DIR_TO_NEXT) {
                send_done = 1;
            } else if (ev.type == PG_WR_TYPE_RECV_CTRL && ev.qp_dir == PG_QP_DIR_FROM_PREV) {
                if (ev.msg.type == msg_type) {
                    recv_done = 1;
                } else {
                    pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
                }
            } else if (ev.type == PG_WR_TYPE_RECV_CTRL) {
                pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
            }
        }
    } else {
        int recv_done = 0;
        if (pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, (int)msg_type, (uint32_t)-1, &ev)) {
            recv_done = 1;
        }

        while (!recv_done) {
            rc = pg_progress_wait(ctx, PG_CTRL_POLL_TIMEOUT_SEC, &ev);
            if (rc != 1) return rc < 0 ? rc : PG_ERR_TIMEOUT;

            if (ev.type == PG_WR_TYPE_RECV_CTRL && ev.qp_dir == PG_QP_DIR_FROM_PREV) {
                if (ev.msg.type == msg_type) {
                    recv_done = 1;
                } else {
                    pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
                }
            } else if (ev.type == PG_WR_TYPE_RECV_CTRL) {
                pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
            }
        }

        /* Forward token to next rank */
        struct pg_ctrl_msg msg;
        memset(&msg, 0, sizeof(msg));
        msg.tag = PG_CTRL_TAG;
        msg.type = msg_type;
        msg.sender_rank = (uint16_t)ctx->rank;

        rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &msg);
        if (rc != PG_SUCCESS) return rc;

        int send_done = 0;
        while (!send_done) {
            rc = pg_progress_wait(ctx, PG_CTRL_POLL_TIMEOUT_SEC, &ev);
            if (rc != 1) return rc < 0 ? rc : PG_ERR_TIMEOUT;

            if (ev.type == PG_WR_TYPE_SEND_CTRL && ev.qp_dir == PG_QP_DIR_TO_NEXT) {
                send_done = 1;
            } else if (ev.type == PG_WR_TYPE_RECV_CTRL) {
                pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
            }
        }
    }

    return PG_SUCCESS;
}

/* Distributed Ring Barrier (3-phase Collect, Release & Acknowledge) */
int pg_barrier(void *pg_handle) {
    if (!pg_handle) return PG_ERR_INVAL;
    struct pg_context *ctx = (struct pg_context *)pg_handle;

    /* Phase 1: Collect (Rank 0 circulates token, ensuring all ranks entered barrier) */
    int rc = pg_barrier_token_pass(ctx, PG_CTRL_MSG_BARRIER_COLLECT);
    if (rc != PG_SUCCESS) return rc;

    /* Phase 2: Release (Rank 0 circulates release token, ensuring all ranks start exit) */
    rc = pg_barrier_token_pass(ctx, PG_CTRL_MSG_BARRIER_RELEASE);
    if (rc != PG_SUCCESS) return rc;

    /* Phase 3: Ack (Rank 0 circulates ack token, ensuring all ranks exited release phase) */
    rc = pg_barrier_token_pass(ctx, PG_CTRL_MSG_BARRIER_ACK);
    if (rc != PG_SUCCESS) return rc;

    return PG_SUCCESS;
}

int connect_process_group(char *servername, void **pg_handle) {
    if (!servername || !pg_handle) {
        fprintf(stderr, "[pg] Error: Invalid NULL argument to connect_process_group\n");
        return PG_ERR_INVAL;
    }

    if (g_pg_args.size < 2) {
        fprintf(stderr, "[pg] Error: Process group size (%d) must be at least 2\n", g_pg_args.size);
        return PG_ERR_INVAL;
    }

    if (g_pg_args.rank < 0 || g_pg_args.rank >= g_pg_args.size) {
        fprintf(stderr, "[pg] Error: Local rank %d out of bounds [0, %d)\n",
                g_pg_args.rank, g_pg_args.size);
        return PG_ERR_INVAL;
    }

    /* Validate servername matches our host list at rank */
    if (strncmp(servername, g_pg_args.hosts[g_pg_args.rank], PG_MAX_HOST_LEN) != 0) {
        fprintf(stderr, "[pg] Error: servername '%s' does not match list[%d] ('%s')\n",
                servername, g_pg_args.rank, g_pg_args.hosts[g_pg_args.rank]);
        return PG_ERR_INVAL;
    }

    struct pg_context *ctx = (struct pg_context *)calloc(1, sizeof(struct pg_context));
    if (!ctx) {
        perror("[pg] Error allocating pg_context");
        return PG_ERR_NOMEM;
    }

    ctx->rank = g_pg_args.rank;
    ctx->size = g_pg_args.size;
    ctx->prev_rank = (ctx->rank - 1 + ctx->size) % ctx->size;
    ctx->next_rank = (ctx->rank + 1) % ctx->size;

    snprintf(ctx->servername, sizeof(ctx->servername), "%s", servername);

    for (int i = 0; i < ctx->size; i++) {
        snprintf(ctx->host_list[i], sizeof(ctx->host_list[i]), "%s", g_pg_args.hosts[i]);
    }

    /* Initialize runtime tuning hyperparameters from environment or defaults (V10) */
    pg_init_tuning_params(ctx);

    /* 1. Allocate RDMA resources (PD, CQ, QPs in INIT state, pre-posted recvs) */
    int rc = pg_rdma_init_resources(ctx);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg] Error: RDMA resource initialization failed with code %d\n", rc);
        pg_rdma_cleanup(ctx);
        free(ctx);
        *pg_handle = NULL;
        return rc;
    }

    /* 2. Run TCP bootstrap: exchange real QP metadata and transition QPs to RTR and RTS */
    rc = pg_tcp_bootstrap(ctx);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg] Error: TCP bootstrap and QP transition failed with code %d\n", rc);
        pg_rdma_cleanup(ctx);
        free(ctx);
        *pg_handle = NULL;
        return rc;
    }

    /* 3. Run distributed RDMA ring barrier to verify ring connectivity and synchronize ranks */
    rc = pg_barrier((void *)ctx);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg] Error: RDMA control ring barrier failed with code %d\n", rc);
        pg_rdma_cleanup(ctx);
        free(ctx);
        *pg_handle = NULL;
        return rc;
    }

    *pg_handle = (void *)ctx;
    return PG_SUCCESS;
}

int pg_close(void *pg_handle) {
    if (!pg_handle) {
        return PG_SUCCESS;
    }

    struct pg_context *ctx = (struct pg_context *)pg_handle;
    /* Synchronize ranks before tearing down RDMA resources */
    pg_barrier(pg_handle);

    pg_rdma_cleanup(ctx);
    free(ctx);
    return PG_SUCCESS;
}

int pg_get_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->rank;
}

int pg_get_size(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->size;
}

int pg_get_prev_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->prev_rank;
}

int pg_get_next_rank(void *pg_handle) {
    if (!pg_handle) return -1;
    return ((struct pg_context *)pg_handle)->next_rank;
}

const char *pg_get_hostname(void *pg_handle, int rank) {
    if (!pg_handle) return NULL;
struct pg_context *ctx = (struct pg_context *)pg_handle;
    if (rank < 0 || rank >= ctx->size) return NULL;
    return ctx->host_list[rank];
}

/* ========================================================================= */
/* === MODULE 6: RING STEP TRANSFER & COLLECTIVES ORCHESTRATION         === */
/* ========================================================================= */

struct pg_reduce_cb_ctx {
    DATATYPE datatype;
    OPERATION op;
    int use_streaming_stores;
    int elem_size;
};

static void pg_reduce_chunk_cb(void *dest, const void *src, size_t len, void *user_ctx) {
    struct pg_reduce_cb_ctx *rctx = (struct pg_reduce_cb_ctx *)user_ctx;
    int micro_elems = (int)(len / (size_t)rctx->elem_size);
    pg_reduce_buffer(dest, src, micro_elems, rctx->datatype, rctx->op, rctx->use_streaming_stores);
}

static void pg_allgather_eager_cb(void *dest, const void *src, size_t len, void *user_ctx) {
    (void)user_ctx;
    memcpy(dest, src, len);
}

/* Eager Payload Ring Step Transfer Engine */
static int pg_ring_step_transfer_eager(struct pg_context *ctx, const struct pg_ring_step_desc *desc) {
    uint32_t num_send_micros = (uint32_t)((desc->send_bytes + ctx->pipeline_chunk - 1) / ctx->pipeline_chunk);
    uint32_t num_recv_micros = (uint32_t)((desc->recv_bytes + ctx->pipeline_chunk - 1) / ctx->pipeline_chunk);

    int send_done = (num_send_micros == 0) ? 1 : 0;
    int recv_done = (num_recv_micros == 0) ? 1 : 0;
    uint32_t eager_posted_micros = 0;
    uint32_t eager_completed_micros = 0;
    uint32_t eager_recv_micros = 0;

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!send_done || !recv_done) {
        /* 1. Pop any pending eager payloads for this recv_tag */
        struct pg_progress_event peager;
        while (!recv_done && pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, PG_CTRL_MSG_EAGER_PAYLOAD, desc->recv_tag, &peager)) {
            uint32_t k = peager.msg.payload.rdv.micro_idx;
            uint32_t micro_len = peager.msg.payload.rdv.length;
            size_t offset = (size_t)k * ctx->pipeline_chunk;

            if (desc->on_recv_chunk) {
                void *dest = (char *)desc->cb_dest + offset;
                const void *src = peager.eager_buf + PG_CTRL_MSG_LEN;
                desc->on_recv_chunk(dest, src, micro_len, desc->cb_user_ctx);
            }

            eager_recv_micros++;
            if (eager_recv_micros == num_recv_micros) {
                recv_done = 1;
            }
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        /* 2. Post eager sends within flow control window */
        while (eager_posted_micros < num_send_micros &&
               (eager_posted_micros - eager_completed_micros) < (uint32_t)ctx->eager_window) {
            uint32_t k = eager_posted_micros;
            size_t offset = (size_t)k * ctx->pipeline_chunk;
            size_t micro_len = desc->send_bytes - offset;
            if (micro_len > ctx->pipeline_chunk) micro_len = ctx->pipeline_chunk;

            void *local_src = (char *)desc->send_buf + offset;

            struct pg_ctrl_msg ehdr;
            memset(&ehdr, 0, sizeof(ehdr));
            ehdr.tag = PG_CTRL_TAG;
            ehdr.type = PG_CTRL_MSG_EAGER_PAYLOAD;
            ehdr.sender_rank = (uint16_t)ctx->rank;
            ehdr.seq = desc->step_idx + 1;
            ehdr.payload.rdv.seg_idx = desc->send_tag;
            ehdr.payload.rdv.micro_idx = k;
            ehdr.payload.rdv.length = (uint32_t)micro_len;

            int rc = pg_post_eager_send(ctx, PG_QP_DIR_TO_NEXT, &ehdr, local_src,
                                        (uint32_t)micro_len, desc->send_lkey, 1,
                                        (int)(desc->step_idx * (num_send_micros > 0 ? num_send_micros : 1) + k));
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "[pg_transfer] Rank %d failed to post eager SEND for micro %u\n", ctx->rank, k);
                return rc;
            }
            eager_posted_micros++;
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        if (send_done && recv_done) break;

        /* 3. Poll CQ completions */
        struct pg_progress_event ev;
        int rc = pg_progress_poll(ctx, &ev);
        if (rc < 0) return rc;
        if (rc == 1) {
            clock_gettime(CLOCK_MONOTONIC, &start);
            switch (ev.type) {
                case PG_WR_TYPE_EAGER_SEND: {
                    if (ev.qp_dir == PG_QP_DIR_TO_NEXT) {
                        eager_completed_micros++;
                        if (eager_completed_micros == num_send_micros) {
                            send_done = 1;
                        }
                    }
                    break;
                }

                case PG_WR_TYPE_RECV_CTRL: {
                    if (ev.qp_dir == PG_QP_DIR_FROM_PREV) {
                        if (ev.msg.tag == PG_CTRL_TAG && ev.msg.type == PG_CTRL_MSG_EAGER_PAYLOAD &&
                            ev.msg.payload.rdv.seg_idx == desc->recv_tag) {
                            uint32_t k = ev.msg.payload.rdv.micro_idx;
                            uint32_t micro_len = ev.msg.payload.rdv.length;
                            size_t offset = (size_t)k * ctx->pipeline_chunk;

                            if (desc->on_recv_chunk) {
                                void *dest = (char *)desc->cb_dest + offset;
                                const void *src = ev.eager_buf + PG_CTRL_MSG_LEN;
                                desc->on_recv_chunk(dest, src, micro_len, desc->cb_user_ctx);
                            }

                            eager_recv_micros++;
                            if (eager_recv_micros == num_recv_micros) {
                                recv_done = 1;
                            }
                        } else if (ev.msg.tag == PG_CTRL_TAG) {
                            pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
                        }
                    } else {
                        pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
                    }
                    break;
                }

                default:
                    break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)PG_CTRL_POLL_TIMEOUT_SEC) {
            fprintf(stderr, "[pg_transfer] Rank %d eager timed out on step %u: "
                            "send_done=%d (%u/%u), recv_done=%d (%u/%u)\n",
                    ctx->rank, desc->step_idx, send_done, eager_completed_micros, num_send_micros,
                    recv_done, eager_recv_micros, num_recv_micros);
            return PG_ERR_TIMEOUT;
        }
    }

    return PG_SUCCESS;
}

/* Rendezvous Multi-WR Pipelined Ring Step Transfer Engine */
static int pg_ring_step_transfer_rdv(struct pg_context *ctx, const struct pg_ring_step_desc *desc) {
    uint32_t num_send_micros = (uint32_t)((desc->send_bytes + ctx->pipeline_chunk - 1) / ctx->pipeline_chunk);
    uint32_t num_recv_micros = (uint32_t)((desc->recv_bytes + ctx->pipeline_chunk - 1) / ctx->pipeline_chunk);

    int send_done = (num_send_micros == 0) ? 1 : 0;
    int recv_done = (num_recv_micros == 0) ? 1 : 0;
    int cts_received = 0;
    uint64_t remote_target_addr = 0;
    uint32_t remote_target_rkey = 0;

    uint32_t rdma_posted_micros = 0;
    uint32_t rdma_completed_micros = 0;
    uint32_t data_done_sent_micros = 0;
    uint32_t data_done_sent_count = 0;
    uint32_t data_done_recv_micros = 0;
    uint32_t send_ctrl_completed_to_next = 0;
    uint32_t send_ctrl_completed_from_prev = 0;

    /* Post RTS to next rank on qp_to_next if we have data to send */
    if (num_send_micros > 0) {
        struct pg_ctrl_msg rts_msg;
        memset(&rts_msg, 0, sizeof(rts_msg));
        rts_msg.tag = PG_CTRL_TAG;
        rts_msg.type = PG_CTRL_MSG_RTS;
        rts_msg.sender_rank = (uint16_t)ctx->rank;
        rts_msg.seq = desc->step_idx + 1;
        rts_msg.payload.rdv.seg_idx = desc->send_tag;
        rts_msg.payload.rdv.length = (uint32_t)desc->send_bytes;

        int rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &rts_msg);
        if (rc != PG_SUCCESS) {
            fprintf(stderr, "[pg_transfer] Rank %d failed to send RTS for step %u\n", ctx->rank, desc->step_idx);
            return rc;
        }
    }

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!send_done || !recv_done) {
        /* 1. Check pending RTS messages from prev */
        struct pg_progress_event pmsg;
        if (num_recv_micros > 0 && pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, PG_CTRL_MSG_RTS, desc->recv_tag, &pmsg)) {
            struct pg_ctrl_msg cts_msg;
            memset(&cts_msg, 0, sizeof(cts_msg));
            cts_msg.tag = PG_CTRL_TAG;
            cts_msg.type = PG_CTRL_MSG_CTS;
            cts_msg.sender_rank = (uint16_t)ctx->rank;
            cts_msg.seq = pmsg.msg.seq;
            cts_msg.payload.rdv.remote_addr = (uint64_t)(uintptr_t)desc->recv_target_addr;
            cts_msg.payload.rdv.rkey = desc->recv_rkey;
            cts_msg.payload.rdv.seg_idx = pmsg.msg.payload.rdv.seg_idx;
            cts_msg.payload.rdv.length = pmsg.msg.payload.rdv.length;

            int rc = pg_post_ctrl_send(ctx, PG_QP_DIR_FROM_PREV, &cts_msg);
            if (rc != PG_SUCCESS) {
                fprintf(stderr, "[pg_transfer] Rank %d failed to send CTS\n", ctx->rank);
                return rc;
            }
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        /* 2. Check pending DATA_DONE messages from prev */
        while (!recv_done && pg_progress_pop_pending(ctx, PG_QP_DIR_FROM_PREV, PG_CTRL_MSG_DATA_DONE, desc->recv_tag, &pmsg)) {
            uint32_t target_micros = pmsg.msg.payload.rdv.micro_idx;
            if (target_micros > num_recv_micros) target_micros = num_recv_micros;
            while (data_done_recv_micros < target_micros) {
                uint32_t k = data_done_recv_micros;
                size_t offset = (size_t)k * ctx->pipeline_chunk;
                size_t micro_len = desc->recv_bytes - offset;
                if (micro_len > ctx->pipeline_chunk) micro_len = ctx->pipeline_chunk;

                if (desc->on_recv_chunk) {
                    void *dest = (char *)desc->cb_dest + offset;
                    const void *src = (char *)desc->recv_target_addr + offset;
                    desc->on_recv_chunk(dest, src, micro_len, desc->cb_user_ctx);
                }
                data_done_recv_micros++;
            }
            if (data_done_recv_micros == num_recv_micros && (send_ctrl_completed_from_prev >= 1 || num_recv_micros == 0)) {
                recv_done = 1;
            }
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        /* 3. Check pending CTS messages from next */
        if (!cts_received && pg_progress_pop_pending(ctx, PG_QP_DIR_TO_NEXT, PG_CTRL_MSG_CTS, desc->send_tag, &pmsg)) {
            cts_received = 1;
            remote_target_addr = pmsg.msg.payload.rdv.remote_addr;
            remote_target_rkey = pmsg.msg.payload.rdv.rkey;
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        /* 4. Post batched RDMA Writes within window */
        while (cts_received && rdma_posted_micros < num_send_micros &&
               (rdma_posted_micros - rdma_completed_micros) < (uint32_t)ctx->rdma_window) {
            uint32_t in_flight = rdma_posted_micros - rdma_completed_micros;
            uint32_t win_avail = (uint32_t)ctx->rdma_window - in_flight;
            uint32_t remaining = num_send_micros - rdma_posted_micros;
            uint32_t to_post = win_avail < remaining ? win_avail : remaining;
            if (to_post > (uint32_t)ctx->batch_size) to_post = (uint32_t)ctx->batch_size;
            if (to_post > 64) to_post = 64;
            if (to_post == 0) break;

            struct ibv_sge sges[64];
            struct ibv_send_wr wrs[64];
            memset(wrs, 0, sizeof(struct ibv_send_wr) * to_post);

            for (uint32_t b = 0; b < to_post; b++) {
                uint32_t k = rdma_posted_micros + b;
                size_t offset = (size_t)k * ctx->pipeline_chunk;
                size_t micro_len = desc->send_bytes - offset;
                if (micro_len > ctx->pipeline_chunk) micro_len = ctx->pipeline_chunk;

                void *local_src = (char *)desc->send_buf + offset;
                uint64_t remote_addr = remote_target_addr + offset;

                int is_signaled = ((k + 1) % ctx->rdma_signal_interval == 0 || (k + 1) == num_send_micros);

                sges[b].addr   = (uintptr_t)local_src;
                sges[b].length = (uint32_t)micro_len;
                sges[b].lkey   = desc->send_lkey;

                wrs[b].wr_id      = pg_make_wr_slot(PG_QP_DIR_TO_NEXT, PG_WR_TYPE_RDMA_WRITE, k);
                wrs[b].opcode     = IBV_WR_RDMA_WRITE;
                wrs[b].send_flags = is_signaled ? IBV_SEND_SIGNALED : 0;
                wrs[b].sg_list    = &sges[b];
                wrs[b].num_sge    = 1;
                wrs[b].next       = (b + 1 < to_post) ? &wrs[b + 1] : NULL;
                wrs[b].wr.rdma.remote_addr = remote_addr;
                wrs[b].wr.rdma.rkey        = remote_target_rkey;
            }

            struct ibv_send_wr *bad_wr = NULL;
            if (ibv_post_send(ctx->qp_to_next, &wrs[0], &bad_wr)) {
                perror("[pg_transfer] Error: ibv_post_send failed for batched RDMA Write");
                return PG_ERR_RDMA;
            }
            rdma_posted_micros += to_post;
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        if (send_done && recv_done) break;

        /* 5. Progress polling */
        struct pg_progress_event ev;
        int rc = pg_progress_poll(ctx, &ev);
        if (rc < 0) return rc;
        if (rc == 1) {
            clock_gettime(CLOCK_MONOTONIC, &start);
            switch (ev.type) {
                case PG_WR_TYPE_RECV_CTRL: {
                    if (ev.msg.tag != PG_CTRL_TAG) {
                        fprintf(stderr, "[pg_transfer] Rank %d invalid control tag 0x%08x\n",
                                ctx->rank, ev.msg.tag);
                        return PG_ERR_RDMA;
                    }

                    if (ev.msg.type == PG_CTRL_MSG_RTS && ev.qp_dir == PG_QP_DIR_FROM_PREV &&
                        ev.msg.payload.rdv.seg_idx == desc->recv_tag && num_recv_micros > 0) {
                        struct pg_ctrl_msg cts_msg;
                        memset(&cts_msg, 0, sizeof(cts_msg));
                        cts_msg.tag = PG_CTRL_TAG;
                        cts_msg.type = PG_CTRL_MSG_CTS;
                        cts_msg.sender_rank = (uint16_t)ctx->rank;
                        cts_msg.seq = ev.msg.seq;
                        cts_msg.payload.rdv.remote_addr = (uint64_t)(uintptr_t)desc->recv_target_addr;
                        cts_msg.payload.rdv.rkey = desc->recv_rkey;
                        cts_msg.payload.rdv.seg_idx = ev.msg.payload.rdv.seg_idx;
                        cts_msg.payload.rdv.length = ev.msg.payload.rdv.length;

                        rc = pg_post_ctrl_send(ctx, PG_QP_DIR_FROM_PREV, &cts_msg);
                        if (rc != PG_SUCCESS) {
                            fprintf(stderr, "[pg_transfer] Rank %d failed to send CTS\n", ctx->rank);
                            return rc;
                        }
                    } else if (ev.msg.type == PG_CTRL_MSG_CTS && ev.qp_dir == PG_QP_DIR_TO_NEXT &&
                               ev.msg.payload.rdv.seg_idx == desc->send_tag && !cts_received) {
                        cts_received = 1;
                        remote_target_addr = ev.msg.payload.rdv.remote_addr;
                        remote_target_rkey = ev.msg.payload.rdv.rkey;
                    } else if (ev.msg.type == PG_CTRL_MSG_DATA_DONE && ev.qp_dir == PG_QP_DIR_FROM_PREV &&
                               ev.msg.payload.rdv.seg_idx == desc->recv_tag && !recv_done) {
                        uint32_t target_micros = ev.msg.payload.rdv.micro_idx;
                        if (target_micros > num_recv_micros) target_micros = num_recv_micros;
                        while (data_done_recv_micros < target_micros) {
                            uint32_t k = data_done_recv_micros;
                            size_t offset = (size_t)k * ctx->pipeline_chunk;
                            size_t micro_len = desc->recv_bytes - offset;
                            if (micro_len > ctx->pipeline_chunk) micro_len = ctx->pipeline_chunk;

                            if (desc->on_recv_chunk) {
                                void *dest = (char *)desc->cb_dest + offset;
                                const void *src = (char *)desc->recv_target_addr + offset;
                                desc->on_recv_chunk(dest, src, micro_len, desc->cb_user_ctx);
                            }
                            data_done_recv_micros++;
                        }
                        if (data_done_recv_micros == num_recv_micros && (send_ctrl_completed_from_prev >= 1 || num_recv_micros == 0)) {
                            recv_done = 1;
                        }
                    } else {
                        pg_progress_push_pending(ctx, ev.qp_dir, &ev.msg, ev.eager_buf);
                    }
                    break;
                }

                case PG_WR_TYPE_RDMA_WRITE: {
                    if (ev.qp_dir == PG_QP_DIR_TO_NEXT) {
                        uint32_t k = ev.slot;
                        if (k + 1 > rdma_completed_micros) {
                            rdma_completed_micros = k + 1;
                        }

                        if (rdma_completed_micros == num_send_micros && data_done_sent_micros < num_send_micros) {
                            struct pg_ctrl_msg done_msg;
                            memset(&done_msg, 0, sizeof(done_msg));
                            done_msg.tag = PG_CTRL_TAG;
                            done_msg.type = PG_CTRL_MSG_DATA_DONE;
                            done_msg.sender_rank = (uint16_t)ctx->rank;
                            done_msg.seq = desc->step_idx + 1;
                            done_msg.payload.rdv.seg_idx = desc->send_tag;
                            done_msg.payload.rdv.micro_idx = num_send_micros;
                            done_msg.payload.rdv.length = (uint32_t)desc->send_bytes;

                            rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &done_msg);
                            if (rc != PG_SUCCESS) {
                                fprintf(stderr, "[pg_transfer] Rank %d failed to send DATA_DONE for micro %u\n", ctx->rank, rdma_completed_micros);
                                return rc;
                            }
                            data_done_sent_micros = num_send_micros;
                            data_done_sent_count++;
                        } else if (rdma_completed_micros > data_done_sent_micros) {
                            struct pg_ctrl_msg done_msg;
                            memset(&done_msg, 0, sizeof(done_msg));
                            done_msg.tag = PG_CTRL_TAG;
                            done_msg.type = PG_CTRL_MSG_DATA_DONE;
                            done_msg.sender_rank = (uint16_t)ctx->rank;
                            done_msg.seq = desc->step_idx + 1;
                            done_msg.payload.rdv.seg_idx = desc->send_tag;
                            done_msg.payload.rdv.micro_idx = rdma_completed_micros;
                            done_msg.payload.rdv.length = (uint32_t)desc->send_bytes;

                            rc = pg_post_ctrl_send(ctx, PG_QP_DIR_TO_NEXT, &done_msg);
                            if (rc != PG_SUCCESS) {
                                fprintf(stderr, "[pg_transfer] Rank %d failed to send DATA_DONE for micro %u\n", ctx->rank, rdma_completed_micros);
                                return rc;
                            }
                            data_done_sent_micros = rdma_completed_micros;
                            data_done_sent_count++;
                        }
                    }
                    break;
                }

                case PG_WR_TYPE_SEND_CTRL: {
                    if (ev.qp_dir == PG_QP_DIR_TO_NEXT) {
                        send_ctrl_completed_to_next++;
                        if (rdma_completed_micros == num_send_micros &&
                            data_done_sent_micros == num_send_micros &&
                            send_ctrl_completed_to_next >= (1 + data_done_sent_count)) {
                            send_done = 1;
                        }
                    } else if (ev.qp_dir == PG_QP_DIR_FROM_PREV) {
                        send_ctrl_completed_from_prev++;
                        if (data_done_recv_micros == num_recv_micros &&
                            send_ctrl_completed_from_prev >= 1) {
                            recv_done = 1;
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)PG_CTRL_POLL_TIMEOUT_SEC) {
            fprintf(stderr, "[pg_transfer] Rank %d timed out on step %u:\n"
                            "  send_done=%d (cts=%d, rdma_post=%u/%u, rdma_comp=%u/%u, done_sent=%u/%u)\n"
                            "  recv_done=%d (done_recv=%u/%u)\n",
                    ctx->rank, desc->step_idx, send_done, cts_received, rdma_posted_micros, num_send_micros,
                    rdma_completed_micros, num_send_micros, data_done_sent_micros, num_send_micros,
                    recv_done, data_done_recv_micros, num_recv_micros);
            return PG_ERR_TIMEOUT;
        }
    }

    return PG_SUCCESS;
}

/* Dispatch transfer step to Eager or Rendezvous engine */
static inline int pg_ring_step_transfer(struct pg_context *ctx, int is_eager, const struct pg_ring_step_desc *desc) {
    if (is_eager) {
        return pg_ring_step_transfer_eager(ctx, desc);
    } else {
        return pg_ring_step_transfer_rdv(ctx, desc);
    }
}

int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op,
                      void *pg_handle) {
    if (!pg_handle || !sendbuf || !recvbuf || count <= 0) {
        return PG_ERR_INVAL;
    }
    struct pg_context *ctx = (struct pg_context *)pg_handle;

    if (datatype != PG_INT && datatype != PG_FLOAT && datatype != PG_DOUBLE) {
        return PG_ERR_UNSUPPORTED;
    }
    if (op != PG_SUM && op != PG_MIN && op != PG_MAX && op != PG_PROD) {
        return PG_ERR_UNSUPPORTED;
    }

    size_t elem_size = pg_get_datatype_size(datatype);
    size_t total_bytes = (size_t)count * elem_size;

    /* Single rank degenerate ring: copy input directly to output */
    if (ctx->size == 1) {
        if (recvbuf != sendbuf) {
            memcpy(recvbuf, sendbuf, total_bytes);
        }
        return PG_SUCCESS;
    }

    /* Maximum segment byte size across ranks */
    int max_seg_elems = pg_get_seg_count(0, count, ctx->size);
    size_t max_seg_bytes = (size_t)max_seg_elems * elem_size;

    /* Ensure internal staging (and safe-mode work) buffers */
    int rc = pg_ensure_internal_buffers(ctx, total_bytes, max_seg_bytes);
    if (rc != PG_SUCCESS) return rc;

    void *work_ptr = NULL;
    struct ibv_mr *work_mr = NULL;

#ifdef PG_WORKBUFFER_INPLACE
    work_ptr = sendbuf;
    work_mr = pg_get_or_reg_mr(ctx, sendbuf, total_bytes,
                               IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!work_mr) {
        fprintf(stderr, "[pg_reduce_scatter] Rank %d failed to register inplace sendbuf MR\n", ctx->rank);
        return PG_ERR_RDMA;
    }
#else
    /* Safe mode: copy sendbuf into internal working buffer */
    if (sendbuf != ctx->work_buf) {
        memcpy(ctx->work_buf, sendbuf, total_bytes);
    }
    work_ptr = ctx->work_buf;
    work_mr = ctx->work_mr;
#endif

    int is_eager = 0;
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
    is_eager = 1;
#elif (PG_ACTIVE_MODE == PG_MODE_TYPE_AUTO)
    if (max_seg_bytes <= ctx->eager_threshold) {
        is_eager = 1;
    }
#endif

    struct pg_reduce_cb_ctx rctx = {
        .datatype = datatype,
        .op = op,
        .use_streaming_stores = ctx->use_streaming_stores,
        .elem_size = (int)elem_size
    };

    /* Execute size - 1 ring reduction steps */
    for (int step = 0; step < ctx->size - 1; step++) {
        int send_seg = (ctx->rank - step - 1 + ctx->size) % ctx->size;
        int recv_seg = (ctx->rank - step - 2 + ctx->size) % ctx->size;

        int send_seg_elems = pg_get_seg_count(send_seg, count, ctx->size);
        size_t send_seg_bytes = (size_t)send_seg_elems * elem_size;
        size_t send_seg_offset = pg_get_seg_offset_bytes(send_seg, count, ctx->size, elem_size);

        int recv_seg_elems = pg_get_seg_count(recv_seg, count, ctx->size);
        size_t recv_seg_bytes = (size_t)recv_seg_elems * elem_size;
        size_t recv_seg_offset = pg_get_seg_offset_bytes(recv_seg, count, ctx->size, elem_size);

        struct pg_ring_step_desc desc;
        memset(&desc, 0, sizeof(desc));
        desc.step_idx = (uint32_t)step;
        desc.send_tag = (uint32_t)send_seg;
        desc.send_buf = (char *)work_ptr + send_seg_offset;
        desc.send_bytes = send_seg_bytes;
        desc.send_lkey = work_mr->lkey;

        desc.recv_tag = (uint32_t)recv_seg;
        desc.recv_target_addr = ctx->staging_buf;
        desc.recv_rkey = ctx->staging_mr->rkey;
        desc.recv_bytes = recv_seg_bytes;

        desc.on_recv_chunk = pg_reduce_chunk_cb;
        desc.cb_dest = (char *)work_ptr + recv_seg_offset;
        desc.cb_user_ctx = &rctx;

        rc = pg_ring_step_transfer(ctx, is_eager, &desc);
        if (rc != PG_SUCCESS) return rc;
    }

    /* Deliver locally owned segment to recvbuf */
    int my_seg_elems = pg_get_seg_count(ctx->rank, count, ctx->size);
    size_t my_seg_bytes = (size_t)my_seg_elems * elem_size;
    size_t my_seg_offset = pg_get_seg_offset_bytes(ctx->rank, count, ctx->size, elem_size);
    if (recvbuf != (char *)work_ptr + my_seg_offset) {
        memcpy(recvbuf, (char *)work_ptr + my_seg_offset, my_seg_bytes);
    }

    return PG_SUCCESS;
}

/* Ring All-Gather Generalized Engine (Zero-Copy RDMA Write into final recvbuf with Multi-WR Batching) */
int pg_ring_all_gather_generalized(struct pg_context *ctx, void *recvbuf, int count, DATATYPE datatype) {
    if (!ctx || !recvbuf || count <= 0) return PG_ERR_INVAL;

    if (ctx->size == 1) return PG_SUCCESS;

    size_t elem_size = pg_get_datatype_size(datatype);
    int max_seg_elems = pg_get_seg_count(0, count, ctx->size);
    size_t max_seg_bytes = (size_t)max_seg_elems * elem_size;
    (void)max_seg_bytes;
    size_t total_bytes = (size_t)count * elem_size;

    struct ibv_mr *recv_mr = pg_get_or_reg_mr(ctx, recvbuf, total_bytes,
                                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!recv_mr) {
        fprintf(stderr, "[pg_all_gather] Rank %d failed to register recvbuf MR\n", ctx->rank);
        return PG_ERR_RDMA;
    }

    int is_eager = 0;
#if (PG_ACTIVE_MODE == PG_MODE_TYPE_EAGER)
    is_eager = 1;
#elif (PG_ACTIVE_MODE == PG_MODE_TYPE_AUTO)
    if (max_seg_bytes <= ctx->eager_threshold) {
        is_eager = 1;
    }
#endif

    /* Execute size - 1 ring gathering steps */
    for (int step = 0; step < ctx->size - 1; step++) {
        int send_origin = (ctx->rank - step + ctx->size) % ctx->size;
        int recv_origin = (ctx->rank - step - 1 + ctx->size) % ctx->size;

        int send_seg_elems = pg_get_seg_count(send_origin, count, ctx->size);
        size_t send_seg_bytes = (size_t)send_seg_elems * elem_size;
        size_t send_seg_offset = pg_get_seg_offset_bytes(send_origin, count, ctx->size, elem_size);

        int recv_seg_elems = pg_get_seg_count(recv_origin, count, ctx->size);
        size_t recv_seg_bytes = (size_t)recv_seg_elems * elem_size;
        size_t recv_seg_offset = pg_get_seg_offset_bytes(recv_origin, count, ctx->size, elem_size);

        struct pg_ring_step_desc desc;
        memset(&desc, 0, sizeof(desc));
        desc.step_idx = (uint32_t)step;
        desc.send_tag = (uint32_t)send_origin;
        desc.send_buf = (char *)recvbuf + send_seg_offset;
        desc.send_bytes = send_seg_bytes;
        desc.send_lkey = recv_mr->lkey;

        desc.recv_tag = (uint32_t)recv_origin;
        desc.recv_target_addr = (char *)recvbuf + recv_seg_offset;
        desc.recv_rkey = recv_mr->rkey;
        desc.recv_bytes = recv_seg_bytes;

        /* Eager mode uses memcpy; Rendezvous mode does Zero-Copy RDMA Write directly into recvbuf */
        desc.on_recv_chunk = is_eager ? pg_allgather_eager_cb : NULL;
        desc.cb_dest = (char *)recvbuf + recv_seg_offset;
        desc.cb_user_ctx = NULL;

        int rc = pg_ring_step_transfer(ctx, is_eager, &desc);
        if (rc != PG_SUCCESS) return rc;
    }

    return PG_SUCCESS;
}


int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype,
                  void *pg_handle) {
    if (!pg_handle || !sendbuf || !recvbuf || count <= 0) {
        return PG_ERR_INVAL;
    }
    struct pg_context *ctx = (struct pg_context *)pg_handle;

    if (datatype != PG_INT && datatype != PG_FLOAT && datatype != PG_DOUBLE) {
        return PG_ERR_UNSUPPORTED;
    }

    size_t elem_size = pg_get_datatype_size(datatype);
    size_t segment_bytes = (size_t)count * elem_size;

    /* Single rank degenerate ring: copy input directly to output */
    if (ctx->size == 1) {
        if (recvbuf != sendbuf) {
            memcpy(recvbuf, sendbuf, segment_bytes);
        }
        return PG_SUCCESS;
    }

    /* Copy sendbuf into our owned slice within recvbuf (recvbuf[rank]) */
    size_t my_offset = (size_t)ctx->rank * segment_bytes;
    if ((char *)recvbuf + my_offset != sendbuf) {
        memcpy((char *)recvbuf + my_offset, sendbuf, segment_bytes);
    }

    /* Run ring zero-copy RDMA all-gather */
    return pg_ring_all_gather_generalized(ctx, recvbuf, count * ctx->size, datatype);
}

int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op,
                  void *pg_handle) {
    if (!pg_handle || !sendbuf || !recvbuf || count <= 0) {
        return PG_ERR_INVAL;
    }
    struct pg_context *ctx = (struct pg_context *)pg_handle;

    if (datatype != PG_INT && datatype != PG_FLOAT && datatype != PG_DOUBLE) {
        return PG_ERR_UNSUPPORTED;
    }
    if (op != PG_SUM && op != PG_MIN && op != PG_MAX && op != PG_PROD) {
        return PG_ERR_UNSUPPORTED;
    }

    size_t elem_size = pg_get_datatype_size(datatype);
    size_t total_bytes = (size_t)count * elem_size;

    /* Single rank degenerate ring: copy input directly to output */
    if (ctx->size == 1) {
        if (recvbuf != sendbuf) {
            memcpy(recvbuf, sendbuf, total_bytes);
        }
        return PG_SUCCESS;
    }

    /* Offset for local owned slice within recvbuf */
    size_t my_seg_offset = pg_get_seg_offset_bytes(ctx->rank, count, ctx->size, elem_size);

    /* Phase 1: Reduce-Scatter into local owned slice recvbuf[rank] */
    int rc = pg_reduce_scatter(sendbuf, (char *)recvbuf + my_seg_offset,
                               count, datatype, op, pg_handle);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg_all_reduce] Rank %d Reduce-Scatter phase failed with code %d\n",
                ctx->rank, rc);
        return rc;
    }

    /* Phase 2: Distributed barrier before All-Gather phase (ensures phase synchronization and minimizes CQ jitter) */
    rc = pg_barrier(pg_handle);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg_all_reduce] Rank %d intermediate barrier failed with code %d\n",
                ctx->rank, rc);
        return rc;
    }

    /* Phase 3: Direct All-Gather distributing reduced segments across ring */
    rc = pg_ring_all_gather_generalized(ctx, recvbuf, count, datatype);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg_all_reduce] Rank %d All-Gather phase failed with code %d\n",
                ctx->rank, rc);
        return rc;
    }

    return PG_SUCCESS;
}
