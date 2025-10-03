#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 共享内存标识 */
#define SHARED_MEMORY_KEY 0x12345678
#define SHARED_MEMORY_SIZE sizeof(struct shared_weather_data)
#define MAX_HISTORY_COUNT 100

/* 传感器数据类型枚举 */
enum sensor_data_type {
    SENSOR_BME280 = 1,      // BME280传感器数据
    SENSOR_LIGHTRAIN = 2,   // 光强雨量传感器数据
    SENSOR_SYSTEM_STATUS = 3, // 系统状态数据
    SENSOR_GPS = 4          // GPS数据
};

/* BME280传感器数据 */
struct bme280_data{
    uint8_t node_id;
    float temperature;      // 温度 (°C)
    float humidity;         // 湿度 (%)
    float pressure;      // 气压 (p)
    time_t timestamp;
    uint8_t valid;
};

/* 光强雨量传感器数据 */
struct lightrain_data {
    uint8_t node_id;
    float light_intensity;  // 光强 (lx)
    uint8_t rainfall;       // 雨量检测 (%)
    time_t timestamp;
    uint8_t valid;
};


/* 系统状态数据 */
struct system_status_data {
    uint8_t node_id;
    uint8_t bme280_status;
    uint8_t bh1750_status;
    uint8_t rain_sensor_status;
    uint8_t i2c_bus_status;
    uint32_t uptime_seconds;
    uint16_t total_errors;
    time_t timestamp;
    uint8_t valid;
};

/* GPS数据 */
struct gps_data {
    uint8_t node_id;
    char utc[7];            // UTC时间 HHMMSS
    float latitude;         // 纬度
    float longitude;        // 经度
    uint8_t positioning;    // 定位模式
    uint8_t satellites;     // 卫星数量
    float hdop;            // 水平精度因子
    float altitude;        // 海拔高度 (m)
    time_t timestamp;
    uint8_t valid;
};

/* 通用气象数据帧 */
struct weather_frame {
    uint8_t data_type;      // 数据类型 (使用sensor_data_type枚举)
    union {
        struct bme280_data bme280;
        struct lightrain_data lightrain;
        struct system_status_data system_status;
        struct gps_data gps;
    } data;
};

/* 共享内存结构体 */
struct shared_weather_data {
    /* 控制信息 */
    volatile uint32_t magic;           // 魔数，用于验证共享内存有效性
    volatile uint32_t writer_pid;      // 写进程PID
    volatile uint32_t reader_pid;      // 读进程PID
    volatile uint32_t update_counter;  // 数据更新计数器
    volatile uint8_t connection_status; // 连接状态 (0=断开, 1=连接中, 2=已连接)
    
    /* 最新数据 */
    struct weather_frame latest_data;
    
    /* 最新各类型数据 */
    struct bme280_data latest_bme280;
    struct lightrain_data latest_lightrain;
    struct system_status_data latest_system_status;
    struct gps_data latest_gps;
    
    /* 历史数据缓冲区 */
    volatile uint32_t history_write_index;  // 写入索引
    volatile uint32_t history_count;        // 历史数据数量
    struct weather_frame history[MAX_HISTORY_COUNT];
    
    /* 统计信息 */
    volatile uint32_t total_received;       // 总接收帧数
    volatile uint32_t total_errors;         // 总错误帧数
    volatile time_t last_update_time;       // 最后更新时间
    
    /* 各类型数据计数 */
    volatile uint32_t bme280_count;
    volatile uint32_t lightrain_count;
    volatile uint32_t system_status_count;
    volatile uint32_t gps_count;
    
    /* 配置信息 */
    char server_ip[16];     // 服务器IP地址
    uint16_t server_port;   // 服务器端口
    
    /* 错误信息 */
    char last_error[256];   // 最后错误信息
};

/* 魔数定义 */
#define SHARED_MEMORY_MAGIC 0xDEADBEEF

/* 连接状态定义 */
#define CONNECTION_DISCONNECTED 0
#define CONNECTION_CONNECTING   1  
#define CONNECTION_CONNECTED    2

#ifdef __cplusplus
}
#endif


#endif /*SHARED_DATA_H*/