/*
数据转发类函数头文件

2025/9/22 程龙
*/
#ifndef PROTO_H
#define PROTO_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>


#define FRAME_LEN     32
#define ROLE_LEN      2
#define CMD_BME280        0x01
#define CMD_LIGHTRAIN     0x02
#define CMD_SYSTEM_STATUS 0x03
#define CMD_GPS           0x04   // 请求GPS数据


/* 角色头（只在握手阶段发送一次） */
static const uint8_t ROLE_SENDER[ROLE_LEN] = {0xAA, 0x00};  // 发送端
static const uint8_t ROLE_RECVR [ROLE_LEN] = {0xBB, 0x00};  // 接收端
static const uint8_t ROLE_ACK   [ROLE_LEN] = {0x01, 0x01};  // 服务器接受
static const uint8_t ROLE_ERRORB[ROLE_LEN] = {0x99, 0x99};  // 服务器拒绝

/* 帧尾结束符 */
static const uint8_t END_SYMBOL[1] = {0xFF};

static inline ssize_t read_n(int fd, void *buf, size_t n);


/* ================== CRC4校验算法 ================== */
static const uint8_t crc4_table[16] = {
    0x0, 0x3, 0x6, 0x5, 0xC, 0xF, 0xA, 0x9,
    0xB, 0x8, 0xD, 0xE, 0x7, 0x4, 0x1, 0x2
};

uint8_t Calculate_CRC4(uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x0F;
    
    for (uint16_t i = 0; i < len; i++) {
        uint8_t tbl_idx = (crc ^ (data[i] >> 4)) & 0x0F;
        crc = crc4_table[tbl_idx] ^ (data[i] & 0x0F);
        
        tbl_idx = (crc ^ (data[i] & 0x0F)) & 0x0F;
        crc = crc4_table[tbl_idx];
    }
    
    return crc & 0x0F;
}

/*================== 数据包描述/数据包解析函数 ==================*/
int LORA_ParseResponse(uint8_t *buf, uint16_t len)
{

    if (len < 3) return 0;
    // //读取测试数据
    // printf("len:%d\n",len);
    // fprintf(stderr, "收到数据：");
    
    // for (size_t i = 0; i < len; ++i) {
    //     fprintf(stderr, "%02x ", buf[i]);  // 打印每个字节的十六进制值
    // }
    // fprintf(stderr, "\n");

    /* 1、BME280数据包解析: 11字节   温度、湿度、气压 */
    if (len == 11 && buf[10] == END_SYMBOL[0] && buf[1] == CMD_BME280) {
        uint8_t node_id = buf[0];
        int16_t t100 = (buf[2] << 8) | buf[3];
        int16_t p10  = (buf[4] << 8) | buf[5];
        int16_t h100 = (buf[6] << 8) | buf[7];
        uint8_t recv_crc4 = buf[8] & 0x0F;
        uint8_t frame_cs = buf[9];
        
        // 验证帧校验和
        uint8_t calc_cs = 0;
        for (int i = 0; i < 9; i++) calc_cs ^= buf[i];
        if (calc_cs != frame_cs) {
            printf("Node %d BME280 frame checksum error!\r\n", node_id);
            return 0;
        }
        
        // 验证CRC4
        uint8_t crc_data[6];
        crc_data[0] = (t100 >> 8) & 0xFF;
        crc_data[1] = t100 & 0xFF;
        crc_data[2] = (p10 >> 8) & 0xFF;
        crc_data[3] = p10 & 0xFF;
        crc_data[4] = (h100 >> 8) & 0xFF;
        crc_data[5] = h100 & 0xFF;
        
        uint8_t calc_crc4 = Calculate_CRC4(crc_data, 6);
        if (calc_crc4 != recv_crc4) {
            printf("Node %d BME280 CRC4 error! Expected:0x%X, Got:0x%X\r\n", 
                    node_id, calc_crc4, recv_crc4);
            return 0;
        }

        //printf("Node %d -> T=%.2f°C, P=%.1f hPa, H=%.2f%% [CRC4:0x%X OK]\r\n",
        //        node_id, t100/100.0f, p10/10.0f, h100/100.0f, recv_crc4);
        
        return 1;
    }

        /* 光强雨量数据包解析: 8字节 */
    if (len == 8 && buf[7] == END_SYMBOL[0] && buf[1] == CMD_LIGHTRAIN) {
        uint8_t node_id = buf[0];
        int16_t lux10 = (buf[2] << 8) | buf[3];
        uint8_t rain = buf[4];
        uint8_t recv_crc4 = buf[5] & 0x0F;
        uint8_t frame_cs = buf[6];
        
        // 验证帧校验和
        uint8_t calc_cs = 0;
        for (int i = 0; i < 6; i++) calc_cs ^= buf[i];
        if (calc_cs != frame_cs) {
            printf("Node %d LightRain frame checksum error!\r\n", node_id);
            return 0;
        }
        
        // 验证CRC4
        uint8_t crc_data[3];
        crc_data[0] = (lux10 >> 8) & 0xFF;
        crc_data[1] = lux10 & 0xFF;
        crc_data[2] = rain;
        
        uint8_t calc_crc4 = Calculate_CRC4(crc_data, 3);
        if (calc_crc4 != recv_crc4) {
            printf("Node %d LightRain CRC4 error! Expected:0x%X, Got:0x%X\r\n", 
                   node_id, calc_crc4, recv_crc4);
            return 0;
        }

        //printf("Node %d -> Lux=%.1f lx, Rain=%u%% [CRC4:0x%X OK]\r\n",
        //       node_id, lux10/10.0f, rain, recv_crc4);
        
        return 2;
    }

        /* 系统状态数据包解析: 15字节 */
    if (len == 15 && buf[14] == END_SYMBOL[0] && buf[1] == CMD_SYSTEM_STATUS) {
        uint8_t node_id = buf[0];
        uint8_t bme280_status = buf[2];
        uint8_t bh1750_status = buf[3];
        uint8_t rain_sensor_status = buf[4];
        uint8_t i2c_bus_status = buf[5];
        uint32_t uptime_seconds = (buf[6] << 24) | (buf[7] << 16) | (buf[8] << 8) | buf[9];
        uint16_t total_errors = (buf[10] << 8) | buf[11];
        uint8_t frame_cs = buf[13];
        
        // 验证帧校验和
        uint8_t calc_cs = 0;
        for (int i = 0; i < 13; i++) calc_cs ^= buf[i];
        if (calc_cs != frame_cs) {
            printf("Node %d SystemStatus frame checksum error!\r\n", node_id);
            return 0;
        }

        // // 打印系统状态信息
        // printf("=== Node %d System Status ===\r\n", node_id);
        // printf("  BME280: %s, BH1750: %s, Rain: %s, I2C: %s\r\n",
        //        bme280_status == 0 ? "OK" : "ERR",
        //        bh1750_status == 0 ? "OK" : "ERR", 
        //        rain_sensor_status == 0 ? "OK" : "ERR",
        //        i2c_bus_status == 0 ? "OK" : "ERR");
        // printf("  Uptime: %u s, Errors: %u\r\n", uptime_seconds, total_errors);


        return 4;
    }

    /* GPS 数据包解析: 25字节 */
    if (len == 25 && buf[24] == END_SYMBOL[0] && buf[1] == CMD_GPS) {
        uint8_t node_id = buf[0];

        // UTC
        char utc[7] = {0};
        memcpy(utc, &buf[2], 6);

        // 纬度
        int32_t lat1e5 = (buf[8] << 24) | (buf[9] << 16) | (buf[10] << 8) | buf[11];
        float lat = lat1e5 / 1e5f;

        // 经度
        int32_t lon1e5 = (buf[12] << 24) | (buf[13] << 16) | (buf[14] << 8) | buf[15];
        float lon = lon1e5 / 1e5f;

        // 定位模式
        uint8_t positioning = buf[16];

        // 卫星数
        uint8_t sats = buf[17];

        // HDOP
        int16_t hdop10 = (buf[18] << 8) | buf[19];
        float hdop = hdop10 / 10.0f;

        // 海拔
        int16_t alt10 = (buf[20] << 8) | buf[21];
        float alt = alt10 / 10.0f;

        // CRC4
        uint8_t recv_crc4 = buf[22] & 0x0F;

        // 校验和
        uint8_t frame_cs = buf[23];
        uint8_t calc_cs = 0;
        for (int i = 0; i < 23; i++) calc_cs ^= buf[i];
        if (calc_cs != frame_cs) return 0;

        // CRC4 检查
        uint8_t crc_data[20];
        memcpy(crc_data, &buf[2], 20);
        uint8_t calc_crc4 = Calculate_CRC4(crc_data, 20);
        if (calc_crc4 != recv_crc4) return 0;

        //printf("Node %d -> UTC=%s, Lat=%.5f, Lon=%.5f, Alt=%.1f m, Sats=%u, HDOP=%.1f [CRC4:0x%X OK]\r\n",
        //   node_id, utc, lat, lon, alt, sats, hdop, recv_crc4);

        return 4;
    }

}


/* ================== 通用数据解析函数 ================== */
/*1、该函数可以自动识别读取的数据长度
  2、然后根据数据长度来判断数据包类型
  3、根据不同数据包类型执行相应的解析函数 */
int LORA_ReadAndRarse(int fd,uint8_t *buf){
    ssize_t r;
    uint8_t header[2];

    //先读取前两字节(node_id,CMD)
    r = read_n(fd,header,2);
    if(r < 0){
        fprintf(stderr,"LORA_ReadAndRarse read_n 1 err\n");
        return (int)r;// 0=对端关闭，-1=出错
    }
    if(r == 0){
        fprintf(stderr,"发送端关闭连接\n");
        return (int)r;// 0=对端关闭，-1=出错
    }

    uint8_t node_id = header[0];
    uint8_t cmd = header[1];
    // printf("cmd:%02x\n",cmd);

    //根据CMD确定包长
    int expected_len = 0;
    switch (cmd)
    {
    case CMD_BME280:        expected_len = 11; break; // BME280
    case CMD_LIGHTRAIN:     expected_len = 8;  break; // 光强雨量
    case CMD_SYSTEM_STATUS: expected_len = 15; break; // 系统状态
    case CMD_GPS:           expected_len = 25;   break; // GPS数据
    
    default:
        //fprintf(stderr, "Node 0x%02X unknown cmd=0x%02X\n", node_id, cmd);
        return -1;
    }

    //再读取剩下的字节
    // fprintf(stderr, "再读取剩下的字节\n");
    // uint8_t buf[32];  //保证足够长
    buf[0] = node_id;
    buf[1] = cmd;

    r = read_n(fd,buf+2,30);
    if (r == 0){
        printf("发送端关闭连接\n");
        return 0;
    }
    if(r < 0){
        perror("LORA_ReadAndRarse read_n 2");
        return -1;
    }

    // 最后检查结束符
    if (buf[expected_len-1] != END_SYMBOL[0]) {
        fprintf(stderr, "Node %u bad tail for cmd=0x%02X\n", node_id, cmd);
        return -1;
    }

    // fprintf(stderr, "收到数据：");

    // for (size_t i = 0; i < sizeof(buf); ++i) {
    // fprintf(stderr, "%02x ", buf[i]);  // 打印每个字节的十六进制值
    // }
    // fprintf(stderr, "\n");

    //调用解析函数
    return LORA_ParseResponse(buf,expected_len);

}

/* 阻塞读满 n 字节；返回读到的字节数（=n 正常；0 对端关闭；-1 出错） */
static inline ssize_t read_n(int fd, void *buf, size_t n) {
    size_t left = n; uint8_t *p = (uint8_t*)buf;
    while (left > 0) {
        ssize_t r = recv(fd, p, left, 0);
        if (r == 0) return (ssize_t)(n - left);  // 对端关闭
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; left -= (size_t)r;
    }
    return (ssize_t)n;
}

/* 发满 n 字节；返回 n 正常，-1 出错 */
static inline ssize_t send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf; size_t left = n;
    while (left > 0) {
        ssize_t w = send(fd, p, left, 0);
        if (w > 0) { p += w; left -= (size_t)w; continue; }
        if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return -1;
    }
    return (ssize_t)n;
}



#endif /* PROTO_H */
