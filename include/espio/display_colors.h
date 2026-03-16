/**
 * @file display_colors.h
 * @brief Common RGB565 color definitions for display
 */

#ifndef ESPIO_DISPLAY_COLORS_H
#define ESPIO_DISPLAY_COLORS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Common RGB565 colors
 */
#define ESPIO_DISPLAY_COLOR_BLACK       0x0000
#define ESPIO_DISPLAY_COLOR_WHITE       0xFFFF
#define ESPIO_DISPLAY_COLOR_RED         0xF800
#define ESPIO_DISPLAY_COLOR_GREEN       0x07E0
#define ESPIO_DISPLAY_COLOR_BLUE        0x001F
#define ESPIO_DISPLAY_COLOR_YELLOW      0xFFE0
#define ESPIO_DISPLAY_COLOR_CYAN        0x07FF
#define ESPIO_DISPLAY_COLOR_MAGENTA     0xF81F
#define ESPIO_DISPLAY_COLOR_ORANGE      0xFD20
#define ESPIO_DISPLAY_COLOR_GRAY        0x8410
#define ESPIO_DISPLAY_COLOR_DARK_GRAY   0x4208
#define ESPIO_DISPLAY_COLOR_LIGHT_GRAY  0xC618

#ifdef __cplusplus
}
#endif

#endif /* ESPIO_DISPLAY_COLORS_H */
