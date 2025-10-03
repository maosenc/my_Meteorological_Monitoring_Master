#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#include "proto.h"

#define BACKLOG 64
#define MAX_RECV_CLIENTS 128

static volatile int g_running = 1;

/* 维护接收者连接列表，收到一帧就广播 */
typedef struct {
    int fds[MAX_RECV_CLIENTS];
    int count;
    pthread_mutex_t mtx;
} recvr_set_t;

static recvr_set_t g_recvers = {
    .fds = {0}, .count = 0, .mtx = PTHREAD_MUTEX_INITIALIZER
};

/*增加接收者*/
static void add_receiver(int fd) {
    pthread_mutex_lock(&g_recvers.mtx);
    if (g_recvers.count < MAX_RECV_CLIENTS) {
        g_recvers.fds[g_recvers.count++] = fd;
        fprintf(stderr, "[server] receiver added, total=%d\n", g_recvers.count);
    } else {
        fprintf(stderr, "[server] receiver full, closing\n");
        close(fd);
    }
    pthread_mutex_unlock(&g_recvers.mtx);
}

static void remove_receiver_nolock(int idx) {
    int fd = g_recvers.fds[idx];
    if (fd >= 0) close(fd);
    g_recvers.fds[idx] = g_recvers.fds[g_recvers.count - 1];
    g_recvers.count--;
}

static void broadcast_frame(const uint8_t *frame) {
    pthread_mutex_lock(&g_recvers.mtx);
    for (int i = 0; i < g_recvers.count; ) {
        if (send_all(g_recvers.fds[i], frame, FRAME_LEN) != FRAME_LEN) {
            fprintf(stderr, "[server] send to receiver failed, removing\n");
            remove_receiver_nolock(i);
            continue; // do not i++
        }
        ++i;
    }
    pthread_mutex_unlock(&g_recvers.mtx);
}

/* 发送端线程：不断收 FRAME_LEN，然后广播给所有接收端 */
static void *sender_thread(void *arg) {
    int conn_fd = *(int*)arg;
    free(arg);

    uint8_t frame[FRAME_LEN];
    while (g_running) {
        // 数据解析函数 解析收到的数据的类型
        int L_r = LORA_ReadAndRarse(conn_fd,frame);
            if(L_r < 0){
                continue;
            }
            if (L_r == 0){
                break;
            }
        broadcast_frame(frame);
    }
    //fprintf(stderr, "[server] broadcast frame: "); print_hex(frame, FRAME_LEN); fprintf(stderr, "\n");

    
    close(conn_fd);
    return NULL;
}


/* 接收端线程：仅用于保持连接；真正的数据由广播直接 send_all */
static void *receiver_thread(void *arg) {
    int conn_fd = *(int*)arg;
    free(arg);
    uint8_t buf[8];  // 用于接收数据

    /* 这里可阻塞读一些字节用于探活；简单起见只睡眠等待，被广播路径写数据 */
    while (g_running) {
        ssize_t r = recv(conn_fd, buf, sizeof(buf), 0);  // 尝试接收数据
        if (r == 0) {
            // 如果接收到 0，表示对端关闭连接
            fprintf(stderr, "[server] connection closed by receiver\n");
            break;  // 断开连接，退出线程
        } 
        else if (r < 0) {
            // 如果发生错误，则检查是否是连接中断
            if (errno == EPIPE || errno == ECONNRESET) {
                fprintf(stderr, "[server] connection reset by receiver\n");
                break;  // 连接断开，退出线程
            }
            //perror("recv");
            continue;  // 继续检查连接
        /* 简单 sleep，实际 IO 由广播写；当对端关闭时，send 会失败并移除 */
        }
    sleep(1);
    }

    // 退出前移除接收端
    remove_receiver_nolock(conn_fd);
    close(conn_fd);
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int port = 8889;
    if (argc >= 2) port = atoi(argv[1]);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); return 1; }

    fprintf(stderr, "[server] listening on %d\n", port);

    while (g_running) {
        struct sockaddr_in cli; socklen_t len = sizeof(cli);
        int conn_fd = accept(listen_fd, (struct sockaddr*)&cli, &len);
        ssize_t r;
        if (conn_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "[server] connection from %s:%u\n", ip, (unsigned)ntohs(cli.sin_port));

        /* 读取 2 字节角色 */
        uint8_t role[ROLE_LEN];
        r = read_n(conn_fd, role, ROLE_LEN);
        if (r != ROLE_LEN) { perror("[server] read role"); 
            close(conn_fd); fprintf(stderr, "READ ROLE_LEN ERROR, closed\n"); 
            continue; 
        }

        fprintf(stderr, "收到角色数据：");
        for (size_t i = 0; i < ROLE_LEN; ++i) {
            fprintf(stderr, "%02x ", role[i]);  // 打印每个字节的十六进制值
        }
        fprintf(stderr, "\n");

        // //读取测试数据
        // uint8_t b[64];
        // r = recv(conn_fd, b, sizeof(b), 0);
        // fprintf(stderr, "收到数据：");
        // for (size_t i = 0; i < sizeof(b); ++i) {
        //     fprintf(stderr, "%02x ", b[i]);  // 打印每个字节的十六进制值
        // }
        // fprintf(stderr, "\n");

        // sleep(100);

        // while (g_running)
        // {   uint8_t buf[32];
        //     int L_r = LORA_ReadAndRarse(conn_fd,buf);
        //     if(L_r <= 0){
        //         continue;
        //     }
        // }

        /* 判断客户端身份 */
        const uint8_t *resp = ROLE_ACK;
        if ( (memcmp(role, ROLE_SENDER, ROLE_LEN) != 0) &&
             (memcmp(role, ROLE_RECVR , ROLE_LEN) != 0) ) {
            resp = ROLE_ERRORB;
        }
        //if (send_all(conn_fd, resp, ROLE_LEN) != ROLE_LEN) { perror("[server] send ack"); close(conn_fd); continue; }
        if (resp == ROLE_ERRORB) { 
            fprintf(stderr, "[server] unknown role, closed\n"); 
            close(conn_fd); continue; }


        /* 根据角色建线程/登记接收者 */
        if (memcmp(role, ROLE_SENDER, ROLE_LEN) == 0) {
            int *pfd = (int*)malloc(sizeof(int)); *pfd = conn_fd;
            pthread_t th; pthread_create(&th, NULL, sender_thread, pfd);
            pthread_detach(th);
        } else { /* receiver */
            add_receiver(conn_fd);
            int *pfd = (int*)malloc(sizeof(int)); *pfd = conn_fd;
            pthread_t th; pthread_create(&th, NULL, receiver_thread, pfd);
            pthread_detach(th);
        }
    }

    close(listen_fd);
    return 0;
}
