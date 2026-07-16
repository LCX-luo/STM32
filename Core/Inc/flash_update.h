#ifndef __FLASH_UPDATE_H
#define __FLASH_UPDATE_H

#include <stdint.h>
#include <string.h>

/* ======================== Flash 布局 ======================== */
#define APP_ADDR            0x08002000      // App 起始地址（28KB）
#define APP_SIZE            0x00007000
#define STAGING_ADDR        0x08009000      // Staging 起始地址（28KB）
#define STAGING_SIZE        0x00007000
#define STAGING_PAGE_NUM    28              // Staging 页数

#define FLAG_PAGE_ADDR      0x08001C00      // 标志位所在页（独立第7页）
#define UPDATE_FLAG_ADDR    0x08001FFC      // 标志位地址
#define MAGIC_UPDATE_READY  0xA5A5A5A5      // 新固件已就绪
#define MAGIC_NONE          0xFFFFFFFF      // 无更新

/* ======================== 通信协议 ======================== */
#define PKT_HEAD            0xAA
#define PKT_TAIL            0x55
#define PKT_TRIGGER         0xF0            // 触发升级（保留，未用）
#define PKT_READY           0x01            // MCU→PC: 准备好接收
#define PKT_INFO            0x02            // PC→MCU: 固件信息
#define PKT_DATA            0x03            // PC→MCU: 数据块
#define PKT_ACK             0x04            // 双向: 确认
#define PKT_NAK             0x05            // 双向: 拒绝
#define PKT_COMPLETE        0x06            // PC→MCU: 传输完成
#define PKT_OK              0x07            // MCU→PC: 校验通过，准备重启
#define PKT_STATUS          0x08            // MCU→PC: 状态文本

#define ERR_CRC             0x01
#define ERR_SEQ             0x02
#define ERR_FLASH           0x03
#define ERR_SIZE            0x04
#define ERR_UNKNOWN         0x05

#define DATA_PAYLOAD_MAX    256             // 单包最大负载
#define FLASH_BUF_SIZE      2048            // 2KB Flash 写入缓冲区
#define RX_RING_SIZE        1024            // 串口接收 Ring Buffer

/* ======================== Ring Buffer ======================== */
typedef struct {
    volatile uint8_t buffer[RX_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} RingBuffer_t;

void RingBuffer_Init(RingBuffer_t *rb);
uint8_t RingBuffer_Write(RingBuffer_t *rb, uint8_t data);
uint8_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *data);

/* ======================== 帧解析 ======================== */
typedef enum {
    FRAME_WAIT_HEAD, FRAME_WAIT_TYPE, FRAME_WAIT_LEN_H, FRAME_WAIT_LEN_L,
    FRAME_WAIT_SEQ_H, FRAME_WAIT_SEQ_L, FRAME_WAIT_PAYLOAD,
    FRAME_WAIT_CRC_H, FRAME_WAIT_CRC_L, FRAME_WAIT_TAIL,
} FrameState_t;

typedef struct {
    uint8_t type;
    uint16_t len;
    uint16_t seq;
    uint8_t payload[DATA_PAYLOAD_MAX];
    uint8_t complete;     // 1=收到完整帧
    uint8_t crc_ok;       // 1=CRC校验通过
    uint8_t error_code;   // NAK 时用
} Frame_t;

void FrameParser_Init(void);
void FrameParser_Feed(uint8_t byte, Frame_t *out);

/* ======================== CRC16 ======================== */
uint16_t crc16_calc(const uint8_t *data, uint16_t len);

/* ======================== 发送帧 ======================== */
void send_packet(uint8_t type, const uint8_t *payload, uint16_t len, uint16_t seq);

/* ======================== Flash 操作 ======================== */
void erase_staging_area(void);
void write_flash_buffer(uint32_t dst_addr, const uint8_t *data, uint16_t len);
void set_update_flag(void);

/* ======================== FOTA 任务入口 ======================== */
void FlashUpdateTask_Entry(void *arg);

#endif /* __FLASH_UPDATE_H */
