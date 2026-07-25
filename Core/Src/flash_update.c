#include "flash_update.h"
#include "usart.h"
#include "iwdg.h"
#include "stm32f1xx_hal.h"
#include "rtos.h"
#include <stdio.h>

/* LOGI 定义在 main.c 中 */
extern void LOGI(const char *format, ...);
extern Mutex_t *FlashMutex;  /* Flash 互斥锁 | Flash controller mutex */

/* ======================== 全局变量 ======================== */
RingBuffer_t g_rx_ring;
uint8_t g_rx_byte;

/* FOTA 任务的静态缓冲区（避免放在任务栈上导致栈溢出） */
static uint8_t s_staging_buf[FLASH_BUF_SIZE];    // 2KB Flash 写入缓冲
static uint8_t s_tx_buf[6 + DATA_PAYLOAD_MAX + 2 + 1];  // 发送帧缓冲

/* ======================== CRC16-CCITT ======================== */
static uint16_t crc16_update(uint16_t crc, uint8_t data)
{
    crc ^= data;
    for (int i = 0; i < 8; i++) {
        if (crc & 1)
            crc = (crc >> 1) ^ 0x8408;
        else
            crc >>= 1;
    }
    return crc;
}

uint16_t crc16_calc(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++)
        crc = crc16_update(crc, data[i]);
    return crc;
}

/* ======================== Ring Buffer ======================== */
void RingBuffer_Init(RingBuffer_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
}

/* ISR 调用：只写 head */
uint8_t RingBuffer_Write(RingBuffer_t *rb, uint8_t data)
{
    uint16_t next = (uint16_t)((rb->head + 1) % RX_RING_SIZE);
    if (next == rb->tail)
        return 0;  // 满
    rb->buffer[rb->head] = data;
    rb->head = next;
    return 1;
}

/* 任务调用：只读 tail */
uint8_t RingBuffer_Read(RingBuffer_t *rb, uint8_t *data)
{
    if (rb->head == rb->tail)
        return 0;  // 空
    *data = rb->buffer[rb->tail];
    rb->tail = (uint16_t)((rb->tail + 1) % RX_RING_SIZE);
    return 1;
}

/* ======================== 帧解析器（状态机） ======================== */
static FrameState_t g_frame_state = FRAME_WAIT_HEAD;
static uint16_t g_frame_len = 0;
static uint16_t g_frame_seq = 0;
static uint8_t g_frame_type = 0;
static uint16_t g_payload_idx = 0;
static uint16_t g_crc_recv = 0;
static uint8_t g_frame_buf[DATA_PAYLOAD_MAX];

void FrameParser_Init(void)
{
    g_frame_state = FRAME_WAIT_HEAD;
    g_payload_idx = 0;
    g_frame_len = 0;
}

void FrameParser_Feed(uint8_t byte, Frame_t *out)
{
    /* 默认不清除 complete，调用者需在解析完整帧后清理 */
    static uint16_t crc_accum = 0;

    switch (g_frame_state) {
    case FRAME_WAIT_HEAD:
        if (byte == PKT_HEAD) {
            g_frame_state = FRAME_WAIT_TYPE;
            g_payload_idx = 0;
            crc_accum = 0xFFFF;
        }
        break;

    case FRAME_WAIT_TYPE:
        g_frame_type = byte;
        crc_accum = crc16_update(crc_accum, byte);
        g_frame_state = FRAME_WAIT_LEN_H;
        break;

    case FRAME_WAIT_LEN_H:
        g_frame_len = (uint16_t)byte << 8;
        crc_accum = crc16_update(crc_accum, byte);
        g_frame_state = FRAME_WAIT_LEN_L;
        break;

    case FRAME_WAIT_LEN_L:
        g_frame_len |= byte;
        crc_accum = crc16_update(crc_accum, byte);
        if (g_frame_len > DATA_PAYLOAD_MAX) {
            g_frame_state = FRAME_WAIT_HEAD;  // 非法长度，丢弃
        } else {
            g_frame_state = FRAME_WAIT_SEQ_H;
        }
        break;

    case FRAME_WAIT_SEQ_H:
        g_frame_seq = (uint16_t)byte << 8;
        crc_accum = crc16_update(crc_accum, byte);
        g_frame_state = FRAME_WAIT_SEQ_L;
        break;

    case FRAME_WAIT_SEQ_L:
        g_frame_seq |= byte;
        crc_accum = crc16_update(crc_accum, byte);
        if (g_frame_len == 0) {
            g_frame_state = FRAME_WAIT_CRC_H;  // 无负载，直接等 CRC
        } else {
            g_payload_idx = 0;
            g_frame_state = FRAME_WAIT_PAYLOAD;
        }
        break;

    case FRAME_WAIT_PAYLOAD:
        g_frame_buf[g_payload_idx++] = byte;
        crc_accum = crc16_update(crc_accum, byte);
        if (g_payload_idx >= g_frame_len) {
            g_frame_state = FRAME_WAIT_CRC_H;
        }
        break;

    case FRAME_WAIT_CRC_H:
        g_crc_recv = (uint16_t)byte << 8;
        g_frame_state = FRAME_WAIT_CRC_L;
        break;

    case FRAME_WAIT_CRC_L:
        g_crc_recv |= byte;
        g_frame_state = FRAME_WAIT_TAIL;
        break;

    case FRAME_WAIT_TAIL:
        out->type = g_frame_type;
        out->len = g_frame_len;
        out->seq = g_frame_seq;
        if (g_frame_len > 0) {
            memcpy(out->payload, g_frame_buf, g_frame_len);
        }
        out->complete = 1;
        out->crc_ok = (crc_accum == g_crc_recv) ? 1 : 0;
        g_frame_state = FRAME_WAIT_HEAD;
        return;  // 提前返回，不执行后面的清零
    }

    out->complete = 0;
}

/*
 * 为使用现有 PrintQueue，需要与 main.c 中 LogMsg_t 布局一致的结构体。
 * LogMsg_t = { char text[128]; uint16_t len; }
 */
typedef struct { char text[128]; uint16_t len; } FotaMsg_t;
extern Queue_t *PrintQueue;

/* ======================== 发送帧 ======================== */
void send_packet(uint8_t type, const uint8_t *payload, uint16_t len, uint16_t seq)
{
    uint16_t idx = 0;
    uint16_t crc = 0xFFFF;
    FotaMsg_t msg;

    s_tx_buf[idx++] = PKT_HEAD;
    s_tx_buf[idx++] = type;         crc = crc16_update(crc, type);
    s_tx_buf[idx++] = (uint8_t)(len >> 8);  crc = crc16_update(crc, s_tx_buf[idx-1]);
    s_tx_buf[idx++] = (uint8_t)(len & 0xFF); crc = crc16_update(crc, s_tx_buf[idx-1]);
    s_tx_buf[idx++] = (uint8_t)(seq >> 8);   crc = crc16_update(crc, s_tx_buf[idx-1]);
    s_tx_buf[idx++] = (uint8_t)(seq & 0xFF); crc = crc16_update(crc, s_tx_buf[idx-1]);

    if (payload && len > 0) {
        for (uint16_t i = 0; i < len; i++) {
            s_tx_buf[idx] = payload[i];
            crc = crc16_update(crc, s_tx_buf[idx]);
            idx++;
        }
    }

    s_tx_buf[idx++] = (uint8_t)(crc >> 8);
    s_tx_buf[idx++] = (uint8_t)(crc & 0xFF);
    s_tx_buf[idx++] = PKT_TAIL;

    /*
     * 走 PrintQueue → PrintTask → DMA，和 LOGI 共用同一通道。
     * 帧是二进制数据（可能含 0x00），所以用 len 字段记录实际长度，
     * PrintTask 已改为用 rxMsg.len 代替 strlen。
     */
    memcpy(msg.text, s_tx_buf, idx);
    msg.len = idx;
    QueueSend(PrintQueue, &msg);
}

/* ======================== Flash 操作 ======================== */
void erase_staging_area(void)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .NbPages = 1
    };
    uint32_t page_error = 0;

    MutexTake(FlashMutex);
    HAL_FLASH_Unlock();

    for (uint16_t i = 0; i < STAGING_PAGE_NUM; i++) {
        /* 擦除一页（~30ms 内 Flash 总线忙，其他任务无法从 Flash 取指令）*/
        erase.PageAddress = STAGING_ADDR + (i * 1024);
        if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
            LOGI("FOTA: Erase page %d failed!\r\n", i);
        }

        /* 每页擦完后必须做的事： */
        HAL_IWDG_Refresh(&hiwdg);     // 刷新硬件看门狗（300ms 超时）
        taskdelay(10);                 // 让出 CPU，给 PrintTask 等任务发送日志

        /* 每 4 页输出一次进度 */
        if ((i % 4) == 0) {
            LOGI("FOTA: Erasing... %d/%d\r\n", i + 1, STAGING_PAGE_NUM);
        }
    }
    HAL_FLASH_Lock();
    MutexGive(FlashMutex);

    /* 最后输出完成状态 */
    LOGI("FOTA: Erase complete\r\n");
}

void write_flash_buffer(uint32_t dst_addr, const uint8_t *data, uint16_t len)
{
    uint16_t word_count = 0;
    MutexTake(FlashMutex);
    HAL_FLASH_Unlock();
    for (uint16_t i = 0; i < len; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, dst_addr + i, word) != HAL_OK) {
            LOGI("FOTA: Flash write fail @ 0x%08X\r\n", dst_addr + i);
        }
        /* 每 128 个字(~5.8ms)刷新看门狗并让出 CPU，
         * 给按键/呼吸灯等任务执行机会 */
        word_count++;
        if (word_count >= 128) {
            word_count = 0;
            HAL_IWDG_Refresh(&hiwdg);
            taskdelay(3);
        }
    }
    HAL_FLASH_Lock();
    MutexGive(FlashMutex);
}

void erase_flag_page(void)
{
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .PageAddress = FLAG_PAGE_ADDR,
        .NbPages = 1
    };
    uint32_t page_error = 0;
    MutexTake(FlashMutex);
    HAL_FLASH_Unlock();
    HAL_FLASHEx_Erase(&erase, &page_error);
    HAL_FLASH_Lock();
    MutexGive(FlashMutex);
}

void set_update_flag(void)
{
    /* 先擦除标志页（第 7 页，独立页不影响 Bootloader） */
    erase_flag_page();
    /* 写入 MAGIC */
    MutexTake(FlashMutex);
    HAL_FLASH_Unlock();
    uint32_t magic = MAGIC_UPDATE_READY;
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, UPDATE_FLAG_ADDR, magic) != HAL_OK) {
        LOGI("FOTA: Write flag failed!\r\n");
    }
    HAL_FLASH_Lock();
    MutexGive(FlashMutex);
}

/* ======================== FOTA 任务 ======================== */
void FlashUpdateTask_Entry(void *arg)
{
    Frame_t frame;
    /* s_staging_buf/s_tx_buf 为静态全局，不在任务栈上 */

    LOGI("FOTA: Task started, erasing staging...\r\n");

    /* ===== Phase 1: 擦除 Staging 区域 ===== */
    erase_staging_area();

    /* 发 READY */
    send_packet(PKT_READY, NULL, 0, 0);

    /*
     * 首次启动 UART RX 中断接收链。
     * 之后每次收到一个字节，HAL_UART_RxCpltCallback 会自动调用
     * HAL_UART_Receive_IT 继续接收下一个字节。
     * 如果不先调用一次，HAL 的 UART RX 状态机不会启动。
     */
    HAL_UART_Receive_IT(&huart2, &g_rx_byte, 1);

    /* ===== Phase 2: 数据接收循环 ===== */
    uint32_t total_size = 0, file_crc32 = 0, bytes_rcvd = 0;
    uint16_t seq_exp = 0, buf_pos = 0, page_idx = 0;
    uint8_t receiving = 1;
    uint8_t status_counter = 0;
    uint16_t ready_resend = 0;        /* 每 3 秒重发一次 READY */

    FrameParser_Init();

    uint16_t yield_counter = 0;  /* 处理一批字节后主动让出 CPU */

    while (receiving) {
        uint8_t byte;
        if (RingBuffer_Read(&g_rx_ring, &byte)) {
            ready_resend = 0;  /* 收到数据就重置计数器 */
            yield_counter++;
            FrameParser_Feed(byte, &frame);

            if (frame.complete) {
                if (!frame.crc_ok) {
                    send_packet(PKT_NAK, (const uint8_t[]){ERR_CRC}, 1, frame.seq);
                    continue;
                }

                switch (frame.type) {

                case PKT_INFO:
                    /* Payload: [总大小:4][CRC32:4] 大端 */
                    total_size  = ((uint32_t)frame.payload[0] << 24) |
                                  ((uint32_t)frame.payload[1] << 16) |
                                  ((uint32_t)frame.payload[2] << 8)  |
                                  frame.payload[3];
                    file_crc32  = ((uint32_t)frame.payload[4] << 24) |
                                  ((uint32_t)frame.payload[5] << 16) |
                                  ((uint32_t)frame.payload[6] << 8)  |
                                  frame.payload[7];
                    bytes_rcvd = 0; seq_exp = 0; buf_pos = 0; page_idx = 0;
                    LOGI("FOTA: Firmware size=%d, CRC32=0x%08X\r\n", total_size, file_crc32);
                    send_packet(PKT_ACK, (const uint8_t[]){0,0}, 2, frame.seq);
                    break;

                case PKT_DATA:
                    if (frame.seq != seq_exp) {
                        send_packet(PKT_NAK, (const uint8_t[]){ERR_SEQ}, 1, frame.seq);
                        break;
                    }
                    /* 拷贝到缓冲区 */
                    memcpy(s_staging_buf + buf_pos, frame.payload, frame.len);
                    buf_pos += frame.len;
                    bytes_rcvd += frame.len;
                    seq_exp++;

                    /* 缓冲区满或全部收完 → 写 Flash */
                    if (buf_pos >= FLASH_BUF_SIZE || bytes_rcvd >= total_size) {
                        if (buf_pos > 0) {
                            write_flash_buffer(
                                STAGING_ADDR + (page_idx * FLASH_BUF_SIZE),
                                s_staging_buf, buf_pos);
                            page_idx++;
                            buf_pos = 0;
                        }
                    }

                    send_packet(PKT_ACK, (const uint8_t[]){(uint8_t)(frame.seq>>8),
                                                           (uint8_t)(frame.seq&0xFF)}, 2, frame.seq);

                    /* 每接收 4KB 输出一次进度 */
                    status_counter++;
                    if (status_counter >= 16) {  // 16 * 256 = 4KB
                        status_counter = 0;
                        LOGI("FOTA: %d/%d bytes\r\n", bytes_rcvd, total_size);
                    }
                    break;

                case PKT_COMPLETE:
                    if (bytes_rcvd != total_size) {
                        send_packet(PKT_NAK, (const uint8_t[]){ERR_SIZE}, 1, 0);
                        LOGI("FOTA: Size mismatch! rcvd=%d, expect=%d\r\n", bytes_rcvd, total_size);
                        break;
                    }
                    /* 写更新标志位 */
                    set_update_flag();
                    send_packet(PKT_OK, NULL, 0, 0);
                    LOGI("FOTA: Update ready! Rebooting...\r\n");
                    taskdelay(200);  // 确保 DMA 发完 OK 帧
                    //NVIC_SystemReset();
                    LOGI("New firmware download completed, waiting for manual reboot to take effect.\r\n");
                    break;

                default:
                    send_packet(PKT_NAK, (const uint8_t[]){ERR_UNKNOWN}, 1, 0);
                    break;
                }
            }

            /*
             * 每处理约 80 字节后主动让出 CPU + 刷看门狗。
             * 传输期间 Ring Buffer 几乎不空→taskdelay(5) 走不到，
             * 不主动让步会导致按键任务（优先级 2）长时间得不到执行。
             */
            if (yield_counter >= 80) {
                yield_counter = 0;
                HAL_IWDG_Refresh(&hiwdg);
                taskdelay(3);
            }
        } else {
            /* 每 ~3 秒重发一次 READY（方便 PC 工具随时连接）*/
            ready_resend++;
            if (ready_resend >= 600) {  /* 600 * 5ms ≈ 3 秒 */
                ready_resend = 0;
                send_packet(PKT_READY, NULL, 0, 0);
            }
            taskdelay(20);  // 无数据，让出 CPU
        }
    }
}
