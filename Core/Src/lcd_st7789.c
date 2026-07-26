#include "lcd_st7789.h"
#include "lcd_font.h"
#include "spi.h"
#include "gpio.h"
#include "rtos.h"
#include <string.h>

/* ST7789 命令 */
#define SWRESET   0x01
#define SLPOUT    0x11
#define NORON     0x13
#define INVOFF    0x20
#define INVON     0x21
#define DISPOFF   0x28
#define DISPON    0x29
#define CASET     0x2A  /* Column address set */
#define RASET     0x2B  /* Row address set */
#define RAMWR     0x2C  /* Memory write */
#define MADCTL    0x36  /* Memory data access control */
#define COLMOD    0x3A  /* Interface pixel format */
#define FRMCTR1   0xB3  /* Frame rate control */
#define DISSET5   0xB6  /* Display function set */
#define GAMSET    0x26  /* Gamma set */
#define GMCTRP1   0xE0  /* Positive gamma correction */
#define GMCTRN1   0xE1  /* Negative gamma correction */

/* SPI 互斥锁 */
static Semaphore_t *SpiSem = NULL;

/* 写命令/数据 */
static void WriteCmd(uint8_t cmd)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);  /* DC = 0: command */
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
}

static void WriteData(uint8_t data)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);    /* DC = 1: data */
    HAL_SPI_Transmit(&hspi1, &data, 1, HAL_MAX_DELAY);
}

/* 初始化 */
void Lcd_Init(void)
{
    if (SpiSem == NULL)
        SpiSem = SemaphoreCreate(1);

    SemaphoreTake(SpiSem);

    /* === 硬件复位 === */
    HAL_GPIO_WritePin(LCD_RES_GPIO_Port, LCD_RES_Pin, GPIO_PIN_RESET);
    taskdelay(10);
    HAL_GPIO_WritePin(LCD_RES_GPIO_Port, LCD_RES_Pin, GPIO_PIN_SET);
    taskdelay(150);

    /* === 退出睡眠模式（必须先唤醒才能配置寄存器）=== */
    WriteCmd(SLPOUT);
    taskdelay(150);

    /* === 基本配置（在 SWRESET 之后，SLPOUT 之后）=== */
    WriteCmd(COLMOD); WriteData(0x05);    /* RGB565, 16bit */
    WriteCmd(MADCTL); WriteData(0x00);    /* 竖屏, RGB 顺序 */

    /* 帧率 */
    WriteCmd(FRMCTR1); WriteData(0x0A); WriteData(0x14);

    /* Gamma */
    WriteCmd(GAMSET); WriteData(0x01);
    WriteCmd(GMCTRP1);
    uint8_t gamma_pos[] = {0xD0,0x08,0x11,0x08,0x0C,0x15,0x3D,0x33,0x53,0x0C,0x16,0x0B,0x1D,0x3A};
    for (int i = 0; i < 14; i++) WriteData(gamma_pos[i]);
    WriteCmd(GMCTRN1);
    uint8_t gamma_neg[] = {0xD0,0x08,0x11,0x08,0x0C,0x15,0x3D,0x33,0x53,0x0C,0x16,0x0B,0x1D,0x3A};
    for (int i = 0; i < 14; i++) WriteData(gamma_neg[i]);

    /* === 显示开启 === */
    WriteCmd(DISPON);
    taskdelay(50);

    SemaphoreGive(SpiSem);
}

/* 设置窗口 (左上角 x,y, 宽度 w, 高度 h) */
void Lcd_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    SemaphoreTake(SpiSem);

    WriteCmd(CASET);
    WriteData(x >> 8); WriteData(x & 0xFF);
    WriteData((x+w-1) >> 8); WriteData((x+w-1) & 0xFF);

    WriteCmd(RASET);
    WriteData(y >> 8); WriteData(y & 0xFF);
    WriteData((y+h-1) >> 8); WriteData((y+h-1) & 0xFF);

    /* 准备写入像素 */
    WriteCmd(RAMWR);

    SemaphoreGive(SpiSem);
}

/* 全屏填充 */
void Lcd_Fill(uint16_t color)
{
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    uint8_t buf[2] = {hi, lo};

    Lcd_SetWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);

    SemaphoreTake(SpiSem);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);

    for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++)
        HAL_SPI_Transmit(&hspi1, buf, 2, HAL_MAX_DELAY);

    SemaphoreGive(SpiSem);
}

/* 画一个像素 */
void Lcd_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;

    uint8_t data[2] = {color >> 8, color & 0xFF};

    Lcd_SetWindow(x, y, 1, 1);

    SemaphoreTake(SpiSem);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    HAL_SPI_Transmit(&hspi1, data, 2, HAL_MAX_DELAY);
    SemaphoreGive(SpiSem);
}

/* 绘制一个字符 */
void Lcd_DrawChar(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 0x20 || c > 0x7E) return;  /* 只打印可见 ASCII */
    const uint8_t *ch = font_8x12[c - 0x20];
    uint16_t color;

    Lcd_SetWindow(x, y, FONT_W, FONT_H);

    SemaphoreTake(SpiSem);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);

    for (int row = 0; row < FONT_H; row++) {
        for (int col = 0; col < FONT_W; col++) {
            color = (ch[row] & (1 << (7 - col))) ? fg : bg;
            uint8_t hi = color >> 8;
            uint8_t lo = color & 0xFF;
            HAL_SPI_Transmit(&hspi1, &hi, 1, HAL_MAX_DELAY);
            HAL_SPI_Transmit(&hspi1, &lo, 1, HAL_MAX_DELAY);
        }
    }
    SemaphoreGive(SpiSem);
}

/* 绘制字符串 */
void Lcd_DrawString(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        Lcd_DrawChar(x, y, *str, fg, bg);
        x += FONT_W;
        if (x + FONT_W > LCD_WIDTH) { x = 0; y += FONT_H; }
        str++;
    }
}
