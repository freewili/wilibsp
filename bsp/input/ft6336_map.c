#include "ft6336_map.h"
#define SCREEN_W 480
#define SCREEN_H 320

ft_point_t ft6336_clamp_point(int screen_x, int screen_y) {
    int sx = screen_x, sy = screen_y;
    if (sx < 0) sx = 0; else if (sx >= SCREEN_W) sx = SCREEN_W - 1;
    if (sy < 0) sy = 0; else if (sy >= SCREEN_H) sy = SCREEN_H - 1;
    ft_point_t p = { (uint16_t)sx, (uint16_t)sy };
    return p;
}

ft_point_t ft6336_map_point(int chip_x, int chip_y) {
    return ft6336_clamp_point(chip_y,                    // chip Y -> screen X
                              (SCREEN_H - 1) - chip_x);  // chip X -> screen Y
}
