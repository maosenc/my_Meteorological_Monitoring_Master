#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <signal.h>
#include <time.h>
#include "proto.h"
// #include "shared_data.h"

/* 连接状态定义 */
#define CONNECTION_DISCONNECTED 0
#define CONNECTION_CONNECTING   1  
#define CONNECTION_CONNECTED    2

static int g_socket_fd = -1;
static volatile int g_running = 1;

/* ================== 小工具函数 ================== */
static void now_str(char *out, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

//打卡SD设备文件
static FILE* open_sd_file(const char *path){
    FILE *fp = fopen(path, "a");
    if (!fp){
        perror("fopen SD file");
        return NULL;
    }
    // 行缓冲：碰到换行自动 flush；再手动 fsync 保证落盘
    setvbuf(fp, NULL, _IOLBF, 0);
    return fp;
};

static void fsync_file(FILE *fp) {
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
}

/* 将 6 字节 UTC（"hhmmss"）格式化为 "hh:mm:ss"；输入未必是 C 字符串 */
static void format_utc_hhmmss(const uint8_t *six, char *out, size_t n) {
    // 兜底处理非可见字符
    int hh = 0, mm = 0, ss = 0;
    char buf[7];
    for (int i = 0; i < 6; ++i) buf[i] = (char)six[i];
    buf[6] = '\0';
    if (sscanf(buf, "%2d%2d%2d", &hh, &mm, &ss) != 3) {
        snprintf(out, n, "--:--:--");
        return;
    }
    snprintf(out, n, "%02d:%02d:%02d", hh, mm, ss);
}

/* 连接到服务器 */
static int connect_to_server(const char *server_ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {

        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {

        perror("inet_pton");
        close(fd);
        return -1;
    }
    

    printf("[receiver] 正在连接到 %s:%d...\n", server_ip, port);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "连接服务器失败: %s", strerror(errno));

        perror("connect");
        close(fd);
        return -1;
    }
    
    printf("[receiver] 连接成功！\n");
    return fd;
}

/* 执行握手 */
static int perform_handshake(int fd) {
    /* 发送角色头 */
    if (send_all(fd, ROLE_RECVR, ROLE_LEN) != ROLE_LEN) {

        perror("send role");
        return -1;
    }
    printf("[receiver] 握手成功\n");
    return 0;
}

/* ================== 写入一行 CSV ================== */
/*
 * 统一 CSV 字段顺序（含人类时间）：
 * type,node_id,ts_local,extra_fields...
 * 其中 type ∈ {BME280,LightRain,System,GPS}
 */
static void log_bme280(FILE *f, const uint8_t *frame) {
    // frame: [0]=node,[1]=cmd,[2..3]=t100,[4..5]=p10,[6..7]=h100,[8]=crc4低4位,[9]=xor_cs,[10]=0xFF
    uint8_t node_id = frame[0];
    int16_t t100 = (frame[2] << 8) | frame[3];
    int16_t p10  = (frame[4] << 8) | frame[5];
    int16_t h100 = (frame[6] << 8) | frame[7];

    char ts[32]; now_str(ts, sizeof ts);
    fprintf(f, "BME280,%u,%s,%.2f,%.1f,%.2f\n",
            node_id, ts, t100/100.0f, p10/10.0f, h100/100.0f);
}

static void log_lightrain(FILE *f, const uint8_t *frame) {
    // frame: [0]=node,[1]=cmd,[2..3]=lux10,[4]=rain,[5]=crc4低4位,[6]=xor_cs,[7]=0xFF
    uint8_t node_id = frame[0];
    int16_t lux10 = (frame[2] << 8) | frame[3];
    uint8_t rain  = frame[4];

    char ts[32]; now_str(ts, sizeof ts);
    fprintf(f, "LightRain,%u,%s,%.1f,%u\n",
            node_id, ts, lux10/10.0f, rain);
}

static void log_system(FILE *f, const uint8_t *frame) {
    // frame: [0]=node,[1]=cmd,[2]=bme,[3]=bh1750,[4]=rain,[5]=i2c,
    // [6..9]=uptime,[10..11]=total_err,[12]=保留? (见头文件未用),[13]=xor_cs,[14]=0xFF
    uint8_t node_id = frame[0];
    uint8_t bme_ok   = frame[2];
    uint8_t bh_ok    = frame[3];
    uint8_t rain_ok  = frame[4];
    uint8_t i2c_ok   = frame[5];
    uint32_t uptime  = (frame[6] << 24) | (frame[7] << 16) | (frame[8] << 8) | frame[9];
    uint16_t errors  = (frame[10] << 8) | frame[11];

    char ts[32]; now_str(ts, sizeof ts);
    fprintf(f, "System,%u,%s,%u,%u,%u,%u,%u,%u\n",
            node_id, ts,
            (unsigned)(bme_ok==0), (unsigned)(bh_ok==0),
            (unsigned)(rain_ok==0), (unsigned)(i2c_ok==0),
            uptime, errors);
}

static void log_gps(FILE *f, const uint8_t *frame) {
    // frame: [0]=node,[1]=cmd,[2..7]=UTC(6字节),"hhmmss",
    // [8..11]=lat*1e5,[12..15]=lon*1e5,[16]=pos_mode,[17]=sats,
    // [18..19]=hdop*10,[20..21]=alt*10,[22]=crc4低4位,[23]=xor_cs,[24]=0xFF
    uint8_t node_id = frame[0];

    char utc_fmt[16];
    format_utc_hhmmss(&frame[2], utc_fmt, sizeof utc_fmt);

    int32_t lat1e5 = (frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11];
    int32_t lon1e5 = (frame[12] << 24) | (frame[13] << 16) | (frame[14] << 8) | frame[15];

    float lat = lat1e5 / 1e5f;
    float lon = lon1e5 / 1e5f;

    uint8_t pos_mode = frame[16];
    uint8_t sats     = frame[17];
    int16_t hdop10   = (frame[18] << 8) | frame[19];
    int16_t alt10    = (frame[20] << 8) | frame[21];

    float hdop = hdop10 / 10.0f;
    float alt  = alt10 / 10.0f;

    char ts[32]; now_str(ts, sizeof ts);
    fprintf(f, "GPS,%u,%s,%s,%.5f,%.5f,%u,%u,%.1f,%.1f\n",
            node_id, ts, utc_fmt, lat, lon, pos_mode, sats, hdop, alt);
}

/* 接收数据循环 */
static void receive_loop(int fd,FILE *f) {
    uint8_t frame[FRAME_LEN];

	printf("[receiver] 开始数据接收...\n");
    while (g_running) {
        /*读取数据帧*/
		int L_r = LORA_ReadAndRarse(fd,frame);
		if(L_r < 0){
			//fprintf(stderr, "receive_loop LORA_ReadAndRarse\n");
			sleep(5);
            continue;
        }
	
		if(L_r == 0){
			fprintf(stderr, "[receiver] server closed\n");
            break;
        }
        // 注意：LORA_ParseResponse 对 System 与 GPS 都返回 4
        // 所以要根据 frame[1] 也就是 CMD 来区分
        uint8_t cmd = frame[1];
        printf("开始存数据\n");
        switch (cmd) {
            case CMD_BME280:
                log_bme280(f, frame);
                fsync_file(f);
                break;
            case CMD_LIGHTRAIN:
                log_lightrain(f, frame);
                fsync_file(f);
                break;
            case CMD_SYSTEM_STATUS:
                log_system(f, frame);
                fsync_file(f);
                break;
            case CMD_GPS:
                log_gps(f, frame);
                fsync_file(f);
                break;
            default:
                // 理论上不会到达
                break;
        }
    }
}


/* 信号处理函数 */
static void signal_handler(int sig) {
    printf("[receiver] 接收到信号 %d，准备退出...\n", sig);
    g_running = 0;
    
    if (g_socket_fd >= 0) {
        close(g_socket_fd);
        g_socket_fd = -1;
    }
}


int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法：%s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    
    /* 注册信号处理函数 */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    //打卡sd卡文件
    const char *sd_path = "/mnt/SD/Meteorological.txt";
    FILE *file = open_sd_file(sd_path);
    if(!file) return 1;
    
    /* 主循环 - 支持自动重连 */
    while (g_running) {
        /* 连接到服务器 */
        g_socket_fd = connect_to_server(server_ip, port);
        if (g_socket_fd < 0) {
            if (g_running) {
                printf("[receiver] 5秒后重试连接...\n");
                sleep(5);
            }
            continue;
        }
        
        /* 执行握手 */
        if (perform_handshake(g_socket_fd) != 0) {
            close(g_socket_fd);
            g_socket_fd = -1;
            if (g_running) {
                printf("[receiver] 握手失败,5秒后重试...\n");
                sleep(5);
            }
            continue;
        }

        /* 接收数据 */
		//printf("开始接收数据...\n");
        receive_loop(g_socket_fd,file);
        
        /* 关闭连接 */
        if (g_socket_fd >= 0) {
            close(g_socket_fd);
            g_socket_fd = -1;
        }

        /* 如果程序还在运行，等待后重连 */
        if (g_running) {
            printf("[receiver] 连接断开,5秒后重连...\n");
            sleep(5);
        }
    }
    // 关闭文件
    fsync_file(file);
    fclose(file);
    printf("[receiver] 程序正常退出\n");

    return 0;
}
