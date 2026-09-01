#ifndef _SENSOR_PROTOCOL_H_
#define _SENSOR_PROTOCOL_H_

#include <stdint.h>

/* 使用魔数区分不同的包类型 */
#define PKT_MAGIC_IMU  0xAA55
#define PKT_MAGIC_CTRL 0x55AA

/* 传感器数据包 (FreeRTOS -> Linux) */
struct imu_sensor_data {
    uint16_t magic;      /* 必须是 PKT_MAGIC_IMU */
    uint32_t timestamp;  /* 采样时间戳 */
    float accel[3];      /* 加速度 xyz */
    float gyro[3];       /* 角速度 xyz */
    float temperature;   /* 传感器温度 */
} __attribute__((packed)); /* 强制 1 字节对齐，跨系统保命神器！ */

/* 控制指令包 (Linux -> FreeRTOS) */
struct motor_ctrl_cmd {
    uint16_t magic;      /* 必须是 PKT_MAGIC_CTRL */
    uint16_t motor_pwm[4]; /* 4 个电机的控制量 */
} __attribute__((packed));

#endif