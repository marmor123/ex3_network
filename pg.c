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
        if (ctx->ctrl_recv_mr[dir]) {
            ibv_dereg_mr(ctx->ctrl_recv_mr[dir]);
            ctx->ctrl_recv_mr[dir] = NULL;
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

    uint32_t max_send_wr = 64;
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

    /* Register control receive and send buffers */
    for (int dir = 0; dir < 2; dir++) {
        ctx->ctrl_recv_mr[dir] = ibv_reg_mr(ctx->pd, ctx->ctrl_recv_buf[dir],
                                           sizeof(ctx->ctrl_recv_buf[dir]),
                                           IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->ctrl_recv_mr[dir]) {
            fprintf(stderr, "[pg_rdma] Error: Could not register control receive MR for dir %d\n", dir);
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
                    .max_send_sge = 1,
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

        /* Pre-post 32 control receive WRs */
        for (int slot = 0; slot < PG_CTRL_POOL_DEPTH; slot++) {
            struct ibv_sge sge = {
                .addr = (uintptr_t)ctx->ctrl_recv_buf[dir][slot],
                .length = PG_CTRL_MSG_LEN,
                .lkey = ctx->ctrl_recv_mr[dir]->lkey
            };
            struct ibv_recv_wr wr = {
                .wr_id = pg_make_wr(dir, PG_WR_TYPE_RECV_CTRL),
                .sg_list = &sge,
                .num_sge = 1,
                .next = NULL
            };
            struct ibv_recv_wr *bad_wr = NULL;
            if (ibv_post_recv(*qp_ptr, &wr, &bad_wr)) {
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

/* V2: Edge-ordered ring-only TCP bootstrap exchanging real QP metadata */
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
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (!send_done || !recv_done) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(ctx->cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "[pg_rdma] Rank %d ibv_poll_cq error: %d\n", ctx->rank, ne);
            return PG_ERR_RDMA;
        }

        if (ne == 1) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[pg_rdma] Rank %d CQ completion error: %s (%d) on wr_id 0x%lx\n",
                        ctx->rank, ibv_wc_status_str(wc.status), wc.status, (unsigned long)wc.wr_id);
                return PG_ERR_RDMA;
            }

            int wr_type = pg_wr_type(wc.wr_id);
            int qp_dir = pg_wr_qp(wc.wr_id);

            if (wr_type == PG_WR_TYPE_SEND_CTRL && qp_dir == PG_QP_DIR_TO_NEXT) {
                send_done = 1;
            } else if (wr_type == PG_WR_TYPE_RECV_CTRL && qp_dir == PG_QP_DIR_FROM_PREV) {
                struct pg_ctrl_msg *recv_msg = (struct pg_ctrl_msg *)ctx->ctrl_recv_buf[PG_QP_DIR_FROM_PREV][0];
                if (recv_msg->tag != PG_CTRL_TAG) {
                    fprintf(stderr, "[pg_rdma] Rank %d received invalid tag 0x%08x (expected 0x%08x)\n",
                            ctx->rank, recv_msg->tag, PG_CTRL_TAG);
                    return PG_ERR_RDMA;
                }
                if (recv_msg->type != PG_CTRL_MSG_PING) {
                    fprintf(stderr, "[pg_rdma] Rank %d received unexpected msg type %u\n",
                            ctx->rank, recv_msg->type);
                    return PG_ERR_RDMA;
                }
                if (recv_msg->sender_rank != (uint16_t)ctx->prev_rank) {
                    fprintf(stderr, "[pg_rdma] Rank %d received ping from unexpected sender %u (expected %d)\n",
                            ctx->rank, recv_msg->sender_rank, ctx->prev_rank);
                    return PG_ERR_RDMA;
                }

                recv_done = 1;

                /* Repost receive buffer for next control message */
                struct ibv_sge sge = {
                    .addr = (uintptr_t)ctx->ctrl_recv_buf[PG_QP_DIR_FROM_PREV][0],
                    .length = PG_CTRL_MSG_LEN,
                    .lkey = ctx->ctrl_recv_mr[PG_QP_DIR_FROM_PREV]->lkey
                };
                struct ibv_recv_wr wr = {
                    .wr_id = pg_make_wr(PG_QP_DIR_FROM_PREV, PG_WR_TYPE_RECV_CTRL),
                    .sg_list = &sge,
                    .num_sge = 1,
                    .next = NULL
                };
                struct ibv_recv_wr *bad_wr = NULL;
                if (ibv_post_recv(ctx->qp_from_prev, &wr, &bad_wr)) {
                    perror("[pg_rdma] Error reposting recv buffer on qp_from_prev");
                    return PG_ERR_RDMA;
                }
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= (double)PG_CTRL_POLL_TIMEOUT_SEC) {
            fprintf(stderr, "[pg_rdma] Rank %d timed out after %.1f s waiting for ring ping completion (send_done=%d, recv_done=%d)\n",
                    ctx->rank, elapsed, send_done, recv_done);
            return PG_ERR_TIMEOUT;
        }
    }

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

    /* 3. Run symmetric RDMA control ring ping to verify ring connectivity */
    rc = pg_rdma_ring_ping(ctx);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg] Error: RDMA control ring ping failed with code %d\n", rc);
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

int pg_reduce_scatter(void *sendbuf, void *recvbuf, int count,
                      DATATYPE datatype, OPERATION op,
                      void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}

int pg_all_gather(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype,
                  void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}

int pg_all_reduce(void *sendbuf, void *recvbuf, int count,
                  DATATYPE datatype, OPERATION op,
                  void *pg_handle) {
    (void)sendbuf;
    (void)recvbuf;
    (void)count;
    (void)datatype;
    (void)op;
    (void)pg_handle;
    return PG_ERR_UNSUPPORTED;
}

