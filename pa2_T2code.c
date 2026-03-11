#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_EVENTS 64
#define PAYLOAD_SIZE 16
#define DEFAULT_CLIENT_THREADS 4
#define DEFAULT_WINDOW_SIZE 32
#define DEFAULT_TIMEOUT_MS 100

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 100000;
int window_size = DEFAULT_WINDOW_SIZE;
int timeout_ms = DEFAULT_TIMEOUT_MS;

/*
 * Data packet sent from client to server.
 * seq_num is the logical packet sequence number.
 */
typedef struct {
    uint32_t seq_num;
    char data[PAYLOAD_SIZE];
} data_pkt_t;

/*
 * ACK packet sent from server to client.
 * ack_num is the sequence number being acknowledged.
 */
typedef struct {
    uint32_t ack_num;
} ack_pkt_t;

/*
 * Per-sequence-number sender state in the client.
 */
typedef struct {
    data_pkt_t pkt;
    int sent;
    int acked;
    struct timeval send_time;
} window_entry_t;

/*
 * Per-thread data for client threads.
 */
typedef struct {
    int epoll_fd;
    int socket_fd;

    struct sockaddr_in server_addr;

    long tx_cnt;           /* original packets generated/sent */
    long rx_cnt;           /* ACKed packets */
    long lost_pkt_cnt;     /* tx_cnt - rx_cnt */
    long retransmit_cnt;   /* number of retransmitted packets */

    uint32_t base;         /* oldest unACKed seq num */
    uint32_t next_seq;     /* next unsent seq num */

    float request_rate;    /* ACKed packets / sec */
} client_thread_data_t;

static long long timeval_diff_us(const struct timeval *a, const struct timeval *b) {
    long long sec = (long long)b->tv_sec - (long long)a->tv_sec;
    long long usec = (long long)b->tv_usec - (long long)a->tv_usec;
    return sec * 1000000LL + usec;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_packet_once(int sock_fd,
                            const struct sockaddr_in *server_addr,
                            const data_pkt_t *pkt) {
    ssize_t s = sendto(sock_fd,
                       pkt,
                       sizeof(*pkt),
                       0,
                       (const struct sockaddr *)server_addr,
                       sizeof(*server_addr));
    if (s != (ssize_t)sizeof(*pkt)) {
        return -1;
    }
    return 0;
}

static void fill_payload(char data[PAYLOAD_SIZE], uint32_t seq_num) {
    /*
     * Give each packet deterministic contents.
     * Not strictly required, but useful for sanity.
     */
    for (int i = 0; i < PAYLOAD_SIZE; i++) {
        data[i] = 'A' + (char)((seq_num + (uint32_t)i) % 26);
    }
}

static int send_new_packet(client_thread_data_t *data,
                           window_entry_t *window_buf,
                           uint32_t seq_num) {
    window_buf[seq_num].pkt.seq_num = seq_num;
    fill_payload(window_buf[seq_num].pkt.data, seq_num);
    window_buf[seq_num].sent = 1;
    window_buf[seq_num].acked = 0;

    if (send_packet_once(data->socket_fd, &data->server_addr, &window_buf[seq_num].pkt) != 0) {
        return -1;
    }

    gettimeofday(&window_buf[seq_num].send_time, NULL);
    return 0;
}

static int retransmit_packet(client_thread_data_t *data,
                             window_entry_t *window_buf,
                             uint32_t seq_num) {
    if (send_packet_once(data->socket_fd, &data->server_addr, &window_buf[seq_num].pkt) != 0) {
        return -1;
    }

    gettimeofday(&window_buf[seq_num].send_time, NULL);
    data->retransmit_cnt++;
    return 0;
}

void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event ev, events[MAX_EVENTS];
    struct timeval thread_start, thread_end;
    long long timeout_us = (long long)timeout_ms * 1000LL;

    window_entry_t *window_buf = NULL;

    window_buf = (window_entry_t *)calloc((size_t)num_requests, sizeof(window_entry_t));
    if (window_buf == NULL) {
        perror("calloc (window_buf)");
        return NULL;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &ev) == -1) {
        perror("epoll_ctl (client add socket)");
        free(window_buf);
        return NULL;
    }

    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->lost_pkt_cnt = 0;
    data->retransmit_cnt = 0;
    data->base = 0;
    data->next_seq = 0;
    data->request_rate = 0.0f;

    gettimeofday(&thread_start, NULL);

    while (data->base < (uint32_t)num_requests) {
        /*
         * Fill the sending window with new packets.
         */
        while (data->next_seq < (uint32_t)num_requests &&
               data->next_seq < data->base + (uint32_t)window_size) {
            uint32_t seq = data->next_seq;

            if (send_new_packet(data, window_buf, seq) == 0) {
                /*
                 * IMPORTANT: Count only original packets here.
                 * Retransmissions do NOT increment tx_cnt.
                 */
                data->tx_cnt++;
                data->next_seq++;
            } else {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                perror("sendto (client new packet)");
                goto done;
            }
        }

        /*
         * Wait briefly for ACKs.
         * We use a short epoll timeout, and separately check timeout on base.
         */
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, 10);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait (client)");
            goto done;
        }

        /*
         * Process all ACKs currently available.
         */
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == data->socket_fd &&
                (events[i].events & EPOLLIN)) {

                while (1) {
                    ack_pkt_t ack;
                    ssize_t r = recvfrom(data->socket_fd,
                                         &ack,
                                         sizeof(ack),
                                         0,
                                         NULL,
                                         NULL);

                    if (r == (ssize_t)sizeof(ack)) {
                        uint32_t ack_num = ack.ack_num;

                        if (ack_num < (uint32_t)num_requests) {
                            /*
                             * Mark this sequence number ACKed if not already.
                             * Then slide base forward over consecutive ACKed packets.
                             */
                            if (!window_buf[ack_num].acked) {
                                window_buf[ack_num].acked = 1;
                                data->rx_cnt++;
                            }

                            while (data->base < (uint32_t)num_requests &&
                                   window_buf[data->base].acked) {
                                data->base++;
                            }
                        }
                    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (r < 0 && errno == EINTR) {
                        continue;
                    } else {
                        perror("recvfrom (client ack)");
                        goto done;
                    }
                }
            }
        }

        /*
         * Timeout check for the oldest unACKed packet.
         * Go-Back-N retransmits all packets currently in the window.
         */
        if (data->base < data->next_seq) {
            struct timeval now;
            gettimeofday(&now, NULL);

            long long elapsed_us = timeval_diff_us(&window_buf[data->base].send_time, &now);

            if (elapsed_us >= timeout_us) {
                for (uint32_t seq = data->base; seq < data->next_seq; seq++) {
                    if (!window_buf[seq].acked) {
                        if (retransmit_packet(data, window_buf, seq) != 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;
                            }
                            perror("sendto (client retransmit)");
                            goto done;
                        }
                    }
                }
            }
        }
    }

done:
    gettimeofday(&thread_end, NULL);

    {
        long long elapsed_us = timeval_diff_us(&thread_start, &thread_end);
        double elapsed_s = (elapsed_us > 0) ? ((double)elapsed_us / 1000000.0) : 0.0;

        data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;
        if (data->lost_pkt_cnt < 0) {
            data->lost_pkt_cnt = 0;
        }

        if (elapsed_s > 0.0) {
            data->request_rate = (float)((double)data->rx_cnt / elapsed_s);
        } else {
            data->request_rate = 0.0f;
        }
    }

    if (window_buf != NULL) {
        free(window_buf);
    }

    if (data->socket_fd >= 0) {
        close(data->socket_fd);
    }
    if (data->epoll_fd >= 0) {
        close(data->epoll_fd);
    }

    return NULL;
}

void run_client() {
    pthread_t *threads = NULL;
    client_thread_data_t *thread_data = NULL;

    threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)num_client_threads);
    thread_data = (client_thread_data_t *)malloc(sizeof(client_thread_data_t) * (size_t)num_client_threads);

    if (threads == NULL || thread_data == NULL) {
        fprintf(stderr, "malloc failed in run_client\n");
        free(threads);
        free(thread_data);
        exit(1);
    }

    for (int i = 0; i < num_client_threads; i++) {
        memset(&thread_data[i], 0, sizeof(thread_data[i]));
        thread_data[i].socket_fd = -1;
        thread_data[i].epoll_fd = -1;

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) {
            perror("socket (client)");
            free(threads);
            free(thread_data);
            exit(1);
        }

        if (set_nonblocking(thread_data[i].socket_fd) == -1) {
            perror("set_nonblocking (client socket)");
            close(thread_data[i].socket_fd);
            free(threads);
            free(thread_data);
            exit(1);
        }

        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd < 0) {
            perror("epoll_create1 (client)");
            close(thread_data[i].socket_fd);
            free(threads);
            free(thread_data);
            exit(1);
        }

        memset(&thread_data[i].server_addr, 0, sizeof(thread_data[i].server_addr));
        thread_data[i].server_addr.sin_family = AF_INET;
        thread_data[i].server_addr.sin_port = htons((uint16_t)server_port);

        if (inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
            close(thread_data[i].epoll_fd);
            close(thread_data[i].socket_fd);
            free(threads);
            free(thread_data);
            exit(1);
        }

        if (pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]) != 0) {
            perror("pthread_create");
            close(thread_data[i].epoll_fd);
            close(thread_data[i].socket_fd);
            free(threads);
            free(thread_data);
            exit(1);
        }
    }

    long total_tx = 0;
    long total_rx = 0;
    long total_lost = 0;
    long total_retransmit = 0;
    float total_request_rate = 0.0f;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);

        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_pkt_cnt;
        total_retransmit += thread_data[i].retransmit_cnt;
        total_request_rate += thread_data[i].request_rate;

        printf("[Thread %d] tx=%ld rx=%ld lost=%ld retransmit=%ld loss_rate=%.2f%% request_rate=%.2f msg/s\n",
               i,
               thread_data[i].tx_cnt,
               thread_data[i].rx_cnt,
               thread_data[i].lost_pkt_cnt,
               thread_data[i].retransmit_cnt,
               (thread_data[i].tx_cnt > 0)
                   ? (100.0 * (double)thread_data[i].lost_pkt_cnt / (double)thread_data[i].tx_cnt)
                   : 0.0,
               thread_data[i].request_rate);
    }

    printf("\n=== Aggregate Results ===\n");
    printf("TOTAL tx=%ld rx=%ld lost=%ld retransmit=%ld loss_rate=%.2f%%\n",
           total_tx,
           total_rx,
           total_lost,
           total_retransmit,
           (total_tx > 0) ? (100.0 * (double)total_lost / (double)total_tx) : 0.0);
    printf("Total Request Rate: %.2f messages/s\n", total_request_rate);

    free(threads);
    free(thread_data);
}

void run_server() {
    int sock_fd = -1;
    int epoll_fd = -1;
    struct sockaddr_in addr;
    struct epoll_event ev, events[MAX_EVENTS];

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket (server)");
        exit(1);
    }

    {
        int opt = 1;
        if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt (SO_REUSEADDR)");
        }
    }

    if (set_nonblocking(sock_fd) == -1) {
        perror("set_nonblocking (server)");
        close(sock_fd);
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
        close(sock_fd);
        exit(1);
    }

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind (server)");
        close(sock_fd);
        exit(1);
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1 (server)");
        close(sock_fd);
        exit(1);
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = sock_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
        perror("epoll_ctl (server add socket)");
        close(epoll_fd);
        close(sock_fd);
        exit(1);
    }

    printf("UDP GBN server listening on %s:%d\n", server_ip, server_port);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait (server)");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == sock_fd &&
                (events[i].events & EPOLLIN)) {

                while (1) {
                    data_pkt_t pkt;
                    ack_pkt_t ack;
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    ssize_t r = recvfrom(sock_fd,
                                         &pkt,
                                         sizeof(pkt),
                                         0,
                                         (struct sockaddr *)&client_addr,
                                         &client_len);

                    if (r == (ssize_t)sizeof(pkt)) {
                        /*
                         * ACK the exact sequence number received.
                         * This is enough for the client's retransmission logic.
                         */
                        ack.ack_num = pkt.seq_num;

                        ssize_t s = sendto(sock_fd,
                                           &ack,
                                           sizeof(ack),
                                           0,
                                           (struct sockaddr *)&client_addr,
                                           client_len);

                        if (s < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                                continue;
                            }
                            perror("sendto (server ack)");
                        }
                    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (r < 0 && errno == EINTR) {
                        continue;
                    } else if (r >= 0) {
                        /*
                         * Ignore malformed packet sizes.
                         */
                        continue;
                    } else {
                        perror("recvfrom (server)");
                        break;
                    }
                }
            }
        }
    }

    close(epoll_fd);
    close(sock_fd);
}

int main(int argc, char *argv[]) {
    /*
     * server:
     *   ./pa2_binary server <server_ip> <server_port>
     *
     * client:
     *   ./pa2_binary client <server_ip> <server_port> <num_client_threads> <num_requests> [window_size] [timeout_ms]
     */

    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }

        if (server_port <= 0) {
            fprintf(stderr, "Invalid server_port\n");
            return 1;
        }

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) {
            server_ip = argv[2];
        }
        if (argc > 3) {
            server_port = atoi(argv[3]);
        }
        if (argc > 4) {
            num_client_threads = atoi(argv[4]);
        }
        if (argc > 5) {
            num_requests = atoi(argv[5]);
        }
        if (argc > 6) {
            window_size = atoi(argv[6]);
        }
        if (argc > 7) {
            timeout_ms = atoi(argv[7]);
        }

        if (server_port <= 0) {
            fprintf(stderr, "Invalid server_port\n");
            return 1;
        }
        if (num_client_threads <= 0) {
            num_client_threads = DEFAULT_CLIENT_THREADS;
        }
        if (num_requests <= 0) {
            fprintf(stderr, "num_requests must be > 0\n");
            return 1;
        }
        if (window_size <= 0) {
            window_size = DEFAULT_WINDOW_SIZE;
        }
        if (timeout_ms <= 0) {
            timeout_ms = DEFAULT_TIMEOUT_MS;
        }

        run_client();
    } else {
        fprintf(stderr,
                "Usage:\n"
                "  %s server <server_ip> <server_port>\n"
                "  %s client <server_ip> <server_port> <num_client_threads> <num_requests> [window_size] [timeout_ms]\n",
                argv[0], argv[0]);
        return 1;
    }

    return 0;
}
