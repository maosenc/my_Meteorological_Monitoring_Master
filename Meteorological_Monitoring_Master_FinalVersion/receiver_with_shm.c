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
#include <sys/shm.h>
#include <sys/ipc.h>
#include <netinet/in.h>
#include <signal.h>
#include <time.h>
#include "proto.h"
#include "shared_data.h"

/* 全局变量 */
static struct shared_weather_data *g_shared_data = NULL;
static int g_shm_id = -1;
static int g_socket_fd = -1;
static volatile int g_running = 1;

/* 初始化共享内存 */
static int init_shared_memory() {
    /* 创建或获取共享内存 */
    g_shm_id = shmget(SHARED_MEMORY_KEY, SHARED_MEMORY_SIZE, IPC_CREAT | 0666);
    if (g_shm_id == -1) {
        perror("shmget");
        return -1;
    }
    
    /* 连接到共享内存 */
    g_shared_data = (struct shared_weather_data *)shmat(g_shm_id, NULL, 0);
    if (g_shared_data == (void *)-1) {
        perror("shmat");
        return -1;
    }
    
    /* 初始化共享内存数据 */
    if (g_shared_data->magic != SHARED_MEMORY_MAGIC) {
        printf("[receiver] 初始化共享内存...\n");
        memset(g_shared_data, 0, sizeof(struct shared_weather_data));
        g_shared_data->magic = SHARED_MEMORY_MAGIC;
        g_shared_data->writer_pid = getpid();
        g_shared_data->connection_status = CONNECTION_DISCONNECTED;
        g_shared_data->update_counter = 0;
        g_shared_data->history_write_index = 0;
        g_shared_data->history_count = 0;
        g_shared_data->total_received = 0;
        g_shared_data->total_errors = 0;
        strncpy(g_shared_data->last_error, "共享内存已初始化", sizeof(g_shared_data->last_error) - 1);
    } else {
        printf("[receiver] 共享内存已存在，接管控制...\n");
        g_shared_data->writer_pid = getpid();
    }
    
    printf("[receiver] 共享内存初始化成功，ID=%d, 地址=%p\n", g_shm_id, g_shared_data);
    return 0;
}

/* 清理共享内存 */
static void cleanup_shared_memory(void) {
    if (g_shared_data != NULL) {
		// 通知读取进程即将关闭
        g_shared_data->connection_status = CONNECTION_DISCONNECTED;
        g_shared_data->writer_pid = 0;
        snprintf(g_shared_data->last_error, sizeof(g_shared_data->last_error), 
                "接收程序已退出 (PID: %d)", getpid());
        
        if (shmdt(g_shared_data) == -1) {
            perror("shmdt");
        }
        g_shared_data = NULL;
    }
	// 新增：删除共享内存段
    if (g_shm_id != -1) {
        if (shmctl(g_shm_id, IPC_RMID, NULL) == -1) {
            // 如果共享内存已经被其他进程附加，可能无法立即删除
            // 但这没关系，系统会在所有进程都解除附加后自动清理
            perror("shmctl IPC_RMID");
        } else {
            printf("[receiver] 共享内存段已删除，ID=%d\n", g_shm_id);
        }
        g_shm_id = -1;
	}
}

/* 更新连接状态 */
static void update_connection_status(uint8_t status) {
    if (g_shared_data != NULL) {
        g_shared_data->connection_status = status;
        g_shared_data->last_update_time = time(NULL);
    }
}

/* 更新错误信息 */
static void update_error_message(const char *error_msg) {
    if (g_shared_data != NULL && error_msg != NULL) {
        strncpy(g_shared_data->last_error, error_msg, sizeof(g_shared_data->last_error) - 1);
        g_shared_data->last_error[sizeof(g_shared_data->last_error) - 1] = '\0';
        g_shared_data->last_update_time = time(NULL);
    }
}

/* 将数据写入共享内存 */
static void write_data_to_shared_memory(const uint8_t *frame) {
    if (g_shared_data == NULL) return;
    
    uint8_t node_id = frame[0];
    uint8_t cmd = frame[1];
    time_t now = time(NULL);
    
    /* 根据命令类型解析数据并写入共享内存 */
    switch (cmd) {
        case CMD_BME280: {
            if (frame[10] != END_SYMBOL[0]) {
                g_shared_data->total_errors++;
                return;
            }
            
            // 解析BME280数据
            int16_t t100 = (frame[2] << 8) | frame[3];
            int16_t p10 = (frame[4] << 8) | frame[5];
            int16_t h100 = (frame[6] << 8) | frame[7];
            uint8_t recv_crc4 = frame[8] & 0x0F;
            uint8_t frame_cs = frame[9];
            
            // 验证帧校验和
            uint8_t calc_cs = 0;
            for (int i = 0; i < 9; i++) calc_cs ^= frame[i];
            if (calc_cs != frame_cs) {
                //printf("[shared_memory] Node %d BME280 frame checksum error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 验证CRC4
            uint8_t crc_data[6] = {frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]};
            uint8_t calc_crc4 = Calculate_CRC4(crc_data, 6);
            if (calc_crc4 != recv_crc4) {
                //printf("[shared_memory] Node %d BME280 CRC4 error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 更新BME280数据
            g_shared_data->latest_bme280.node_id = node_id;
            g_shared_data->latest_bme280.temperature = t100 / 100.0f;
            g_shared_data->latest_bme280.pressure = p10 / 10.0f;
            g_shared_data->latest_bme280.humidity = h100 / 100.0f;
            g_shared_data->latest_bme280.timestamp = now;
            g_shared_data->latest_bme280.valid = 1;
            
            // 更新通用数据帧
            g_shared_data->latest_data.data_type = SENSOR_BME280;
            g_shared_data->latest_data.data.bme280 = g_shared_data->latest_bme280;
            
            g_shared_data->bme280_count++;
            //printf("[shared_memory] BME280 data updated: Node=%u, T=%.2f°C, P=%.1f hPa, H=%.2f%%\n",
            //       node_id, g_shared_data->latest_bme280.temperature, 
            //       g_shared_data->latest_bme280.pressure, g_shared_data->latest_bme280.humidity);
            break;
        }
        
        case CMD_LIGHTRAIN: {
            if (frame[7] != END_SYMBOL[0]) {
                g_shared_data->total_errors++;
                return;
            }
            
            // 解析光强雨量数据
            int16_t lux10 = (frame[2] << 8) | frame[3];
            uint8_t rain = frame[4];
            uint8_t recv_crc4 = frame[5] & 0x0F;
            uint8_t frame_cs = frame[6];
            
            // 验证帧校验和
            uint8_t calc_cs = 0;
            for (int i = 0; i < 6; i++) calc_cs ^= frame[i];
            if (calc_cs != frame_cs) {
                //printf("[shared_memory] Node %d LightRain frame checksum error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 验证CRC4
            uint8_t crc_data[3] = {frame[2], frame[3], frame[4]};
            uint8_t calc_crc4 = Calculate_CRC4(crc_data, 3);
            if (calc_crc4 != recv_crc4) {
                //printf("[shared_memory] Node %d LightRain CRC4 error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 更新光强雨量数据
            g_shared_data->latest_lightrain.node_id = node_id;
            g_shared_data->latest_lightrain.light_intensity = lux10 / 10.0f;
            g_shared_data->latest_lightrain.rainfall = rain;
            g_shared_data->latest_lightrain.timestamp = now;
            g_shared_data->latest_lightrain.valid = 1;
            
            // 更新通用数据帧
            g_shared_data->latest_data.data_type = SENSOR_LIGHTRAIN;
            g_shared_data->latest_data.data.lightrain = g_shared_data->latest_lightrain;
            
            g_shared_data->lightrain_count++;
            //printf("[shared_memory] LightRain data updated: Node=%u, Lux=%.1f lx, Rain=%u%%\n",
            //       node_id, g_shared_data->latest_lightrain.light_intensity, 
            //       g_shared_data->latest_lightrain.rainfall);
            break;
        }
        
        case CMD_SYSTEM_STATUS: {
            if (frame[14] != END_SYMBOL[0]) {
                g_shared_data->total_errors++;
                return;
            }
            
            // 解析系统状态数据
            uint8_t bme280_status = frame[2];
            uint8_t bh1750_status = frame[3];
            uint8_t rain_sensor_status = frame[4];
            uint8_t i2c_bus_status = frame[5];
            uint32_t uptime_seconds = (frame[6] << 24) | (frame[7] << 16) | (frame[8] << 8) | frame[9];
            uint16_t total_errors = (frame[10] << 8) | frame[11];
            uint8_t frame_cs = frame[13];
            
            // 验证帧校验和
            uint8_t calc_cs = 0;
            for (int i = 0; i < 13; i++) calc_cs ^= frame[i];
            if (calc_cs != frame_cs) {
                //printf("[shared_memory] Node %d SystemStatus frame checksum error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 更新系统状态数据
            g_shared_data->latest_system_status.node_id = node_id;
            g_shared_data->latest_system_status.bme280_status = bme280_status;
            g_shared_data->latest_system_status.bh1750_status = bh1750_status;
            g_shared_data->latest_system_status.rain_sensor_status = rain_sensor_status;
            g_shared_data->latest_system_status.i2c_bus_status = i2c_bus_status;
            g_shared_data->latest_system_status.uptime_seconds = uptime_seconds;
            g_shared_data->latest_system_status.total_errors = total_errors;
            g_shared_data->latest_system_status.timestamp = now;
            g_shared_data->latest_system_status.valid = 1;
            
            // 更新通用数据帧
            g_shared_data->latest_data.data_type = SENSOR_SYSTEM_STATUS;
            g_shared_data->latest_data.data.system_status = g_shared_data->latest_system_status;
            
            g_shared_data->system_status_count++;
            //printf("[shared_memory] SystemStatus data updated: Node=%u, Uptime=%u s, Errors=%u\n",
            //       node_id, uptime_seconds, total_errors);
            break;
        }
        
        case CMD_GPS: {
            if (frame[24] != END_SYMBOL[0]) {
                g_shared_data->total_errors++;
                return;
            }
            
            // 解析GPS数据
            char utc[7] = {0};
            memcpy(utc, &frame[2], 6);
            int32_t lat1e5 = (frame[8] << 24) | (frame[9] << 16) | (frame[10] << 8) | frame[11];
            int32_t lon1e5 = (frame[12] << 24) | (frame[13] << 16) | (frame[14] << 8) | frame[15];
            uint8_t positioning = frame[16];
            uint8_t sats = frame[17];
            int16_t hdop10 = (frame[18] << 8) | frame[19];
            int16_t alt10 = (frame[20] << 8) | frame[21];
            uint8_t recv_crc4 = frame[22] & 0x0F;
            uint8_t frame_cs = frame[23];
            
            // 验证帧校验和
            uint8_t calc_cs = 0;
            for (int i = 0; i < 23; i++) calc_cs ^= frame[i];
            if (calc_cs != frame_cs) {
                //printf("[shared_memory] Node %d GPS frame checksum error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 验证CRC4
            uint8_t crc_data[20];
            memcpy(crc_data, &frame[2], 20);
            uint8_t calc_crc4 = Calculate_CRC4(crc_data, 20);
            if (calc_crc4 != recv_crc4) {
                //printf("[shared_memory] Node %d GPS CRC4 error!\n", node_id);
                g_shared_data->total_errors++;
                return;
            }
            
            // 更新GPS数据
            g_shared_data->latest_gps.node_id = node_id;
            strncpy(g_shared_data->latest_gps.utc, utc, sizeof(g_shared_data->latest_gps.utc) - 1);
            g_shared_data->latest_gps.latitude = lat1e5 / 1e5f;
            g_shared_data->latest_gps.longitude = lon1e5 / 1e5f;
            g_shared_data->latest_gps.positioning = positioning;
            g_shared_data->latest_gps.satellites = sats;
            g_shared_data->latest_gps.hdop = hdop10 / 10.0f;
            g_shared_data->latest_gps.altitude = alt10 / 10.0f;
            g_shared_data->latest_gps.timestamp = now;
            g_shared_data->latest_gps.valid = 1;
            
            // 更新通用数据帧
            g_shared_data->latest_data.data_type = SENSOR_GPS;
            g_shared_data->latest_data.data.gps = g_shared_data->latest_gps;
            
            g_shared_data->gps_count++;
            //printf("[shared_memory] GPS data updated: Node=%u, UTC=%s, Lat=%.5f, Lon=%.5f\n",
            //       node_id, utc, g_shared_data->latest_gps.latitude, g_shared_data->latest_gps.longitude);
            break;
        }
        
        default:
            printf("[shared_memory] Unknown command: 0x%02X\n", cmd);
            g_shared_data->total_errors++;
            return;
    }
    
    /* 添加到历史缓冲区 */
    uint32_t write_idx = g_shared_data->history_write_index;
    g_shared_data->history[write_idx] = g_shared_data->latest_data;
    
    /* 更新写入索引 */
    g_shared_data->history_write_index = (write_idx + 1) % MAX_HISTORY_COUNT;
    
    /* 更新历史数据计数 */
    if (g_shared_data->history_count < MAX_HISTORY_COUNT) {
        g_shared_data->history_count++;
    }
    /* 更新计数器和统计信息 */
    g_shared_data->update_counter++;
    g_shared_data->total_received++;
    g_shared_data->last_update_time = now;
}

/* 连接到服务器 */
static int connect_to_server(const char *server_ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        update_error_message("创建socket失败");
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        update_error_message("无效的IP地址");
        perror("inet_pton");
        close(fd);
        return -1;
    }
    
    update_connection_status(CONNECTION_CONNECTING);
    printf("[receiver] 正在连接到 %s:%d...\n", server_ip, port);
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf), "连接服务器失败: %s", strerror(errno));
        update_error_message(error_buf);
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
        update_error_message("发送角色标识失败");
        perror("send role");
        return -1;
    }
    
    update_connection_status(CONNECTION_CONNECTED);
    update_error_message("连接握手成功");
    printf("[receiver] 握手成功\n");
    return 0;
}

/* 接收数据循环 */
static void receive_loop(int fd) {
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

        /* 将数据写入共享内存 */
        write_data_to_shared_memory(frame);
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

	// 新增：清理共享内存
    cleanup_shared_memory();
}


int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法：%s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    
    const char *server_ip = argv[1];
    int port = atoi(argv[2]);
    
    /* 注册信号处理函数 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 初始化共享内存 */
    if (init_shared_memory() != 0) {
        fprintf(stderr, "[receiver] 共享内存初始化失败\n");
        return 1;
    }
    
    /* 保存服务器信息到共享内存 */
    strncpy(g_shared_data->server_ip, server_ip, sizeof(g_shared_data->server_ip) - 1);
    g_shared_data->server_port = (uint16_t)port;
    
    printf("[receiver] 数据接收程序启动 (PID: %d)\n", getpid());
    printf("[receiver] 共享内存键值: 0x%08X\n", SHARED_MEMORY_KEY);
    
    /* 主循环 - 支持自动重连 */
    while (g_running) {
        /* 连接到服务器 */
        g_socket_fd = connect_to_server(server_ip, port);
        if (g_socket_fd < 0) {
            update_connection_status(CONNECTION_DISCONNECTED);
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
            update_connection_status(CONNECTION_DISCONNECTED);
            if (g_running) {
                printf("[receiver] 握手失败,5秒后重试...\n");
                sleep(5);
            }
            continue;
        }
        
        /* 接收数据 */
		//printf("开始接收数据...\n");
        receive_loop(g_socket_fd);
        
        /* 关闭连接 */
        if (g_socket_fd >= 0) {
            close(g_socket_fd);
            g_socket_fd = -1;
        }
        update_connection_status(CONNECTION_DISCONNECTED);
        
        /* 如果程序还在运行，等待后重连 */
        if (g_running) {
            printf("[receiver] 连接断开,5秒后重连...\n");
            sleep(5);
        }
    }
    
    printf("[receiver] 程序正常退出\n");
    cleanup_shared_memory();
    return 0;
}
