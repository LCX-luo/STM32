#ifndef __LCD_FONT_H
#define __LCD_FONT_H

#include <stdint.h>

#define FONT_W  8  /* 字体宽度 */
#define FONT_H  12 /* 字体高度 */

/* 标准 ASCII 字库 (0x20~0x7E, 95 字符) */
extern const uint8_t font_8x12[95][FONT_H];

#endif
