#ifndef __LCD_ST7789_H
#define __LCD_ST7789_H

#include <stdint.h>

/* LCD 尺寸 */
#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* 颜色定义 (RGB565) */
#define BLACK       0x0000
#define WHITE       0xFFFF
#define RED         0xF800
#define GREEN       0x07E0
#define BLUE        0x001F
#define YELLOW      0xFFE0
#define CYAN        0x07FF
#define MAGENTA     0xF81F
#define GRAY        0x8410

/* 批量填充矩形区域 */
void Lcd_FillRegion(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/* 公开 API */
void Lcd_Init(void);
void Lcd_Reset(void);
void Lcd_SetWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void Lcd_Fill(uint16_t color);
void Lcd_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
void Lcd_DrawChar(int x, int y, char c, uint16_t fg, uint16_t bg);
void Lcd_DrawString(int x, int y, const char *str, uint16_t fg, uint16_t bg);

#endif
