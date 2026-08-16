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

/* V1: Edge-ordered ring-only TCP bootstrap exchanging dummy QP metadata */
int pg_tcp_bootstrap(struct pg_context *ctx) {
    if (!ctx) return PG_ERR_INVAL;

    /* Initialize deterministic dummy QP info for V1 dry-run */
    ctx->local_to_next.qpn = 0x1000 + (ctx->rank << 4) + 1;
    ctx->local_to_next.psn = 0x2000 + (ctx->rank << 4) + 1;
    ctx->local_to_next.lid = (uint16_t)(0x0100 + ctx->rank);
    ctx->local_to_next.reserved = 0;

    ctx->local_from_prev.qpn = 0x1000 + (ctx->rank << 4) + 2;
    ctx->local_from_prev.psn = 0x2000 + (ctx->rank << 4) + 2;
    ctx->local_from_prev.lid = (uint16_t)(0x0100 + ctx->rank);
    ctx->local_from_prev.reserved = 0;

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

    /* Run V1 TCP bootstrap dry-run */
    int rc = pg_tcp_bootstrap(ctx);
    if (rc != PG_SUCCESS) {
        fprintf(stderr, "[pg] Error: TCP bootstrap failed with code %d\n", rc);
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
