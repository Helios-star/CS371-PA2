#include <stdio.h>
#include <stdlib.h>
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
#define MESSAGE_SIZE 16
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
 * Per-thread client stats/data
 */
typedef struct {
    int epoll_fd;
    int socket_fd;

    long long total_rtt;
    long total_messages;
    float request_rate;

    long tx_cnt;
    long rx_cnt;
    long lost_pkt_cnt;

    struct sockaddr_in server_addr;
} client_thread_data_t;

static long long timeval_diff_us(const struct timeval *a, const struct timeval *b) {
    long long sec = (long long)b->tv_sec - (long long)a->tv_sec;
    long long usec = (long long)b->tv_usec - (long long)a->tv_usec;
    return sec * 1000000LL + usec;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * Client thread:
 * Implements UDP pipelining for Task 1.
 * No sequence numbers or retransmission here.
 * We simply send up to window_size packets without waiting for each reply,
 * then use epoll + timeout to receive as many echoes as arrive.
 */
void *client_thread_func(void *arg) {
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event ev, events[MAX_EVENTS];

    static const char send_buf[MESSAGE_SIZE] = {
        'A','B','C','D','E','F','G','H',
        'I','J','K','L','M','N','O','P'
    };

    char recv_buf[2048];

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = data->socket_fd;

    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &ev) == -1) {
        perror("epoll_ctl (client add socket)");
        return NULL;
    }

    data->total_rtt = 0;
    data->total_messages = 0;
    data->request_rate = 0.0f;
    data->tx_cnt = 0;
    data->rx_cnt = 0;
    data->lost_pkt_cnt = 0;

    struct timeval thread_start, thread_end;
    gettimeofday(&thread_start, NULL);

    long sent_so_far = 0;
    long received_so_far = 0;
    int inflight = 0;

    /*
     * We measure RTT very approximately for Task 1 by timing the overall run.
     * Since packets are pipelined and unlabeled, per-packet RTT is not meaningful
     * without sequence numbers. total_messages is still tracked via rx_cnt.
     */
    while (sent_so_far < num_requests || inflight > 0) {
        /*
         * Fill the pipeline window
         */
        while (sent_so_far < num_requests && inflight < window_size) {
            ssize_t s = sendto(data->socket_fd,
                               send_buf,
                               MESSAGE_SIZE,
                               0,
                               (struct sockaddr *)&data->server_addr,
                               sizeof(data->server_addr));

            if (s == MESSAGE_SIZE) {
                data->tx_cnt++;
                sent_so_far++;
                inflight++;
            } else if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            } else {
                perror("sendto (client)");
                goto done;
            }
        }

        /*
         * Wait for echoed packets up to timeout_ms.
         * Any inflight packets still missing after timeout are considered lost
         * for Task 1 measurement purposes.
         */
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, timeout_ms);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait (client)");
            goto done;
        }

        if (nfds == 0) {
            /*
             * Timeout: assume all currently inflight packets not yet received
             * are lost for Task 1. This is the measurement mechanism requested.
             */
            inflight = 0;
            continue;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == data->socket_fd && (events[i].events & EPOLLIN)) {
                while (1) {
                    ssize_t r = recvfrom(data->socket_fd,
                                         recv_buf,
                                         sizeof(recv_buf),
                                         0,
                                         NULL,
                                         NULL);

                    if (r == MESSAGE_SIZE) {
                        data->rx_cnt++;
                        data->total_messages++;
                        received_so_far++;
                        if (inflight > 0) {
                            inflight--;
                        }
                    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (r < 0 && errno == EINTR) {
                        continue;
                    } else if (r >= 0) {
                        /*
                         * Unexpected packet size. Ignore it.
                         */
                        continue;
                    } else {
                        perror("recvfrom (client)");
                        goto done;
                    }
                }
            }
        }
    }

done:
    gettimeofday(&thread_end, NULL);

    long long elapsed_us = timeval_diff_us(&thread_start, &thread_end);
    double elapsed_s = (elapsed_us > 0) ? (elapsed_us / 1000000.0) : 0.0;

    data->lost_pkt_cnt = data->tx_cnt - data->rx_cnt;
    if (data->lost_pkt_cnt < 0) {
        data->lost_pkt_cnt = 0;
    }

    if (elapsed_s > 0.0) {
        data->request_rate = (float)(data->rx_cnt / elapsed_s);
    } else {
        data->request_rate = 0.0f;
    }

    if (data->socket_fd >= 0) close(data->socket_fd);
    if (data->epoll_fd >= 0) close(data->epoll_fd);

    return NULL;
}

void run_client() {
    pthread_t *threads = NULL;
    client_thread_data_t *thread_data = NULL;

    threads = malloc(sizeof(pthread_t) * num_client_threads);
    thread_data = malloc(sizeof(client_thread_data_t) * num_client_threads);
    if (!threads || !thread_data) {
        fprintf(stderr, "malloc failed in run_client\n");
        free(threads);
        free(thread_data);
        exit(1);
    }

    for (int i = 0; i < num_client_threads; i++) {
        memset(&thread_data[i], 0, sizeof(thread_data[i]));

        thread_data[i].socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (thread_data[i].socket_fd < 0) {
            perror("socket (client)");
            free(threads);
            free(thread_data);
            exit(1);
        }

        if (set_nonblocking(thread_data[i].socket_fd) == -1) {
            perror("set_nonblocking (client)");
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
        thread_data[i].server_addr.sin_port = htons(server_port);

        if (inet_pton(AF_INET, server_ip, &thread_data[i].server_addr.sin_addr) != 1) {
            fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
            close(thread_data[i].epoll_fd);
            close(thread_data[i].socket_fd);
            free(threads);
            free(thread_data);
            exit(1);
        }

        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0.0f;
        thread_data[i].tx_cnt = 0;
        thread_data[i].rx_cnt = 0;
        thread_data[i].lost_pkt_cnt = 0;

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
    float total_request_rate = 0.0f;

    for (int i = 0; i < num_client_threads; i++) {
        pthread_join(threads[i], NULL);

        total_tx += thread_data[i].tx_cnt;
        total_rx += thread_data[i].rx_cnt;
        total_lost += thread_data[i].lost_pkt_cnt;
        total_request_rate += thread_data[i].request_rate;

        printf("[Thread %d] tx=%ld rx=%ld lost=%ld",
               i,
               thread_data[i].tx_cnt,
               thread_data[i].rx_cnt,
               thread_data[i].lost_pkt_cnt);

        if (thread_data[i].tx_cnt > 0) {
            double loss_pct = 100.0 *
                ((double)thread_data[i].lost_pkt_cnt / (double)thread_data[i].tx_cnt);
            printf(" loss_rate=%.2f%%", loss_pct);
        }

        printf(" request_rate=%.2f msg/s\n", thread_data[i].request_rate);
    }

    printf("\n=== Aggregate Results ===\n");
    printf("TOTAL tx=%ld rx=%ld lost=%ld", total_tx, total_rx, total_lost);
    if (total_tx > 0) {
        printf(" loss_rate=%.2f%%",
               100.0 * ((double)total_lost / (double)total_tx));
    }
    printf("\n");
    printf("Total Request Rate: %.2f messages/s\n", total_request_rate);

    free(threads);
    free(thread_data);
}

void run_server() {
    int sock_fd = -1;
    int epoll_fd = -1;
    struct sockaddr_in server_addr;
    struct epoll_event ev, events[MAX_EVENTS];

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket (server)");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt (SO_REUSEADDR)");
    }

    if (set_nonblocking(sock_fd) == -1) {
        perror("set_nonblocking (server)");
        close(sock_fd);
        exit(1);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server_ip: %s\n", server_ip);
        close(sock_fd);
        exit(1);
    }

    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
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

    printf("UDP server listening on %s:%d\n", server_ip, server_port);

    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait (server)");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == sock_fd && (events[i].events & EPOLLIN)) {
                while (1) {
                    char buf[MESSAGE_SIZE];
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);

                    ssize_t r = recvfrom(sock_fd,
                                         buf,
                                         sizeof(buf),
                                         0,
                                         (struct sockaddr *)&client_addr,
                                         &client_len);

                    if (r == MESSAGE_SIZE) {
                        ssize_t s = sendto(sock_fd,
                                           buf,
                                           MESSAGE_SIZE,
                                           0,
                                           (struct sockaddr *)&client_addr,
                                           client_len);
                        if (s < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            perror("sendto (server)");
                        }
                    } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    } else if (r < 0 && errno == EINTR) {
                        continue;
                    } else if (r >= 0) {
                        /*
                         * Ignore malformed / short packets
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
     * Usage:
     *   server: ./pa2_binary server <server_ip> <server_port>
     *   client: ./pa2_binary client <server_ip> <server_port> <num_client_threads> <num_requests> [window_size] [timeout_ms]
     */

    if (argc > 1 && strcmp(argv[1], "server") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);

        run_server();
    } else if (argc > 1 && strcmp(argv[1], "client") == 0) {
        if (argc > 2) server_ip = argv[2];
        if (argc > 3) server_port = atoi(argv[3]);
        if (argc > 4) num_client_threads = atoi(argv[4]);
        if (argc > 5) num_requests = atoi(argv[5]);
        if (argc > 6) window_size = atoi(argv[6]);
        if (argc > 7) timeout_ms = atoi(argv[7]);

        if (num_client_threads <= 0) num_client_threads = DEFAULT_CLIENT_THREADS;
        if (num_requests <= 0) num_requests = 100000;
        if (window_size <= 0) window_size = DEFAULT_WINDOW_SIZE;
        if (timeout_ms <= 0) timeout_ms = DEFAULT_TIMEOUT_MS;

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
