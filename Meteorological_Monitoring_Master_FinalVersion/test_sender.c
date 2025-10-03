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
#include <time.h>
#include <math.h>

#include "proto.h"

#define SEND_INTERVAL 3   /* 发送间隔秒数 */

/* 生成BME280数据包 (11字节) */
static void build_bme280_frame(uint8_t *buf, uint8_t node_id) {
    memset(buf, 0, 32);
    
    buf[0] = node_id;          // 节点ID
    buf[1] = CMD_BME280;       // 命令
    
    // 模拟温度 20-30°C，精度0.01°C (int16)
    float temp = 20.0f + (rand() % 1000) / 100.0f;
    int16_t t100 = (int16_t)(temp * 100);
    buf[2] = (t100 >> 8) & 0xFF;
    buf[3] = t100 & 0xFF;
    
    // 模拟气压 1000-1020 hPa，精度0.1 hPa (int16)
    float pressure = 1000.0f + (rand() % 200) / 10.0f;
    int16_t p10 = (int16_t)(pressure * 10);
    buf[4] = (p10 >> 8) & 0xFF;
    buf[5] = p10 & 0xFF;
    
    // 模拟湿度 40-80%，精度0.01% (int16)
    float humidity = 40.0f + (rand() % 4000) / 100.0f;
    int16_t h100 = (int16_t)(humidity * 100);
    buf[6] = (h100 >> 8) & 0xFF;
    buf[7] = h100 & 0xFF;
    
    // 计算CRC4
    uint8_t crc_data[6] = {buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]};
    uint8_t crc4 = Calculate_CRC4(crc_data, 6);
    buf[8] = crc4 & 0x0F;
    
    // 计算帧校验和
    uint8_t checksum = 0;
    for (int i = 0; i < 9; i++) {
        checksum ^= buf[i];
    }
    buf[9] = checksum;
    
    // 结束符
    buf[10] = END_SYMBOL[0];
    
    printf("[sender] BME280: Node=%u, T=%.2f°C, P=%.1f hPa, H=%.2f%%, CRC4=0x%X\n",
           node_id, temp, pressure, humidity, crc4);
}

/* 生成光强雨量数据包 (8字节) */
static void build_lightrain_frame(uint8_t *buf, uint8_t node_id) {
    memset(buf, 0, 32);
    
    buf[0] = node_id;          // 节点ID
    buf[1] = CMD_LIGHTRAIN;    // 命令
    
    // 模拟光强 0-1000 lx，精度0.1 lx (int16)
    float lux = (rand() % 10000) / 10.0f;
    int16_t lux10 = (int16_t)(lux * 10);
    buf[2] = (lux10 >> 8) & 0xFF;
    buf[3] = lux10 & 0xFF;
    
    // 模拟雨量检测 0-100% (uint8)
    uint8_t rain = rand() % 101;
    buf[4] = rain;
    
    // 计算CRC4
    uint8_t crc_data[3] = {buf[2], buf[3], buf[4]};
    uint8_t crc4 = Calculate_CRC4(crc_data, 3);
    buf[5] = crc4 & 0x0F;
    
    // 计算帧校验和
    uint8_t checksum = 0;
    for (int i = 0; i < 6; i++) {
        checksum ^= buf[i];
    }
    buf[6] = checksum;
    
    // 结束符
    buf[7] = END_SYMBOL[0];
    
    printf("[sender] LightRain: Node=%u, Lux=%.1f lx, Rain=%u%%, CRC4=0x%X\n",
           node_id, lux, rain, crc4);
}

/* 生成系统状态数据包 (15字节) */
static void build_system_status_frame(uint8_t *buf, uint8_t node_id, uint32_t *uptime) {
    memset(buf, 0, 32);
    
    buf[0] = node_id;              // 节点ID
    buf[1] = CMD_SYSTEM_STATUS;    // 命令
    
    // 模拟传感器状态 (0=OK, 1=ERR)
    buf[2] = (rand() % 10) ? 0 : 1;  // BME280状态
    buf[3] = (rand() % 10) ? 0 : 1;  // BH1750状态
    buf[4] = (rand() % 10) ? 0 : 1;  // 雨量传感器状态
    buf[5] = (rand() % 10) ? 0 : 1;  // I2C总线状态
    
    // 系统运行时间 (秒)
    (*uptime) += SEND_INTERVAL;
    buf[6] = (*uptime >> 24) & 0xFF;
    buf[7] = (*uptime >> 16) & 0xFF;
    buf[8] = (*uptime >> 8) & 0xFF;
    buf[9] = *uptime & 0xFF;
    
    // 总错误数
    uint16_t total_errors = rand() % 100;
    buf[10] = (total_errors >> 8) & 0xFF;
    buf[11] = total_errors & 0xFF;
    
    // 预留字节
    buf[12] = 0;
    
    // 计算帧校验和
    uint8_t checksum = 0;
    for (int i = 0; i < 13; i++) {
        checksum ^= buf[i];
    }
    buf[13] = checksum;
    
    // 结束符
    buf[14] = END_SYMBOL[0];
    
    printf("[sender] SystemStatus: Node=%u, BME=%s, BH=%s, Rain=%s, I2C=%s, Up=%u s, Err=%u\n",
           node_id, buf[2] ? "ERR" : "OK", buf[3] ? "ERR" : "OK", 
           buf[4] ? "ERR" : "OK", buf[5] ? "ERR" : "OK", *uptime, total_errors);
}

/* 生成GPS数据包 (25字节) */
static void build_gps_frame(uint8_t *buf, uint8_t node_id) {
    memset(buf, 0, 32);
    
    buf[0] = node_id;      // 节点ID
    buf[1] = CMD_GPS;      // 命令
    
    // 模拟UTC时间 HHMMSS
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    sprintf((char*)&buf[2], "%02d%02d%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
    
    // 模拟纬度 (39.9042°N -> 3990420)
    float lat = 39.9042f + (rand() % 1000 - 500) / 100000.0f;
    int32_t lat1e5 = (int32_t)(lat * 1e5);
    buf[8] = (lat1e5 >> 24) & 0xFF;
    buf[9] = (lat1e5 >> 16) & 0xFF;
    buf[10] = (lat1e5 >> 8) & 0xFF;
    buf[11] = lat1e5 & 0xFF;
    
    // 模拟经度 (116.4074°E -> 11640740)
    float lon = 116.4074f + (rand() % 1000 - 500) / 100000.0f;
    int32_t lon1e5 = (int32_t)(lon * 1e5);
    buf[12] = (lon1e5 >> 24) & 0xFF;
    buf[13] = (lon1e5 >> 16) & 0xFF;
    buf[14] = (lon1e5 >> 8) & 0xFF;
    buf[15] = lon1e5 & 0xFF;
    
    // 定位模式 (1=无定位, 2=2D, 3=3D)
    buf[16] = 2 + (rand() % 2);
    
    // 卫星数量
    buf[17] = 4 + (rand() % 8);
    
    // HDOP (精度因子) 0.5-5.0，精度0.1
    float hdop = 0.5f + (rand() % 450) / 100.0f;
    int16_t hdop10 = (int16_t)(hdop * 10);
    buf[18] = (hdop10 >> 8) & 0xFF;
    buf[19] = hdop10 & 0xFF;
    
    // 海拔高度 0-1000m，精度0.1m
    float alt = (rand() % 10000) / 10.0f;
    int16_t alt10 = (int16_t)(alt * 10);
    buf[20] = (alt10 >> 8) & 0xFF;
    buf[21] = alt10 & 0xFF;
    
    // 计算CRC4
    uint8_t crc_data[20];
    memcpy(crc_data, &buf[2], 20);
    uint8_t crc4 = Calculate_CRC4(crc_data, 20);
    buf[22] = crc4 & 0x0F;
    
    // 计算帧校验和
    uint8_t checksum = 0;
    for (int i = 0; i < 23; i++) {
        checksum ^= buf[i];
    }
    buf[23] = checksum;
    
    // 结束符
    buf[24] = END_SYMBOL[0];
    
    printf("[sender] GPS: Node=%u, UTC=%s, Lat=%.5f, Lon=%.5f, Alt=%.1f m, Sats=%u, HDOP=%.1f, CRC4=0x%X\n",
           node_id, (char*)&buf[2], lat, lon, alt, buf[17], hdop, crc4);
}

/* 发送数据包的通用函数 */
static int send_packet(int fd, uint8_t *buf, int len) {
    if (send_all(fd, buf, len) != len) {
        perror("send packet");
        return -1;
    }
    
    // 发送额外的填充数据，使总长度为FRAME_LEN
    if (len < FRAME_LEN) {
        uint8_t padding[FRAME_LEN];
        memset(padding, 0, FRAME_LEN);
        memcpy(padding, buf, len);
        
        if (send_all(fd, padding + len, FRAME_LEN - len) != (FRAME_LEN - len)) {
            perror("send padding");
            return -1;
        }
    }
    
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法：%s <server_ip> <port> [node_id]\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    uint8_t node_id = (argc >= 4) ? (uint8_t)atoi(argv[3]) : 1;  // 默认节点ID为1

    // 初始化随机数种子
    srand((unsigned int)time(NULL));

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { 
        perror("socket"); 
        return 1; 
    }

    struct sockaddr_in addr; 
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        perror("inet_pton"); 
        close(fd); 
        return 1;
    }
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); 
        close(fd); 
        return 1;
    }
    
    printf("[sender] connected to %s:%d, Node ID=%u\n", server_ip, port, node_id);

    /* 握手：发角色头，等服务器处理 */
    if (send_all(fd, ROLE_SENDER, ROLE_LEN) != ROLE_LEN) { 
        perror("send role"); 
        close(fd); 
        return 1; 
    }
    
    printf("[sender] role sent, starting data transmission...\n");

    /* 数据发送循环 */
    uint32_t uptime = 0;
    int packet_count = 0;
    uint8_t buf[32];

    while (1) {
        // 循环发送不同类型的数据包
        switch (packet_count % 4) {
            case 0:
                build_bme280_frame(buf, node_id);
                if (send_packet(fd, buf, 11) < 0) {
                    goto cleanup;
                }
                break;
                
            case 1:
                build_lightrain_frame(buf, node_id);
                if (send_packet(fd, buf, 8) < 0) {
                    goto cleanup;
                }
                break;
                
            case 2:
                build_system_status_frame(buf, node_id, &uptime);
                if (send_packet(fd, buf, 15) < 0) {
                    goto cleanup;
                }
                break;
                
            case 3:
                build_gps_frame(buf, node_id);
                if (send_packet(fd, buf, 25) < 0) {
                    goto cleanup;
                }
                break;
        }
        
        packet_count++;
        printf("[sender] Packet %d sent, sleeping %d seconds...\n\n", packet_count, SEND_INTERVAL);
        sleep(SEND_INTERVAL);
    }

cleanup:
    close(fd);
    return 0;
}