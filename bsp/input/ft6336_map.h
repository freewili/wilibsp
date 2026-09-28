#ifndef FT6336_MAP_H
#define FT6336_MAP_H
#include <stdint.h>
typedef struct { uint16_t x, y; } ft_point_t;
// Map raw FT6336 chip coordinates to screen pixels (480x320 landscape),
// clamped to the panel. chip-Y -> screen X; chip-X -> screen Y (inverted).
ft_point_t ft6336_map_point(int chip_x, int chip_y);

// Hold a screen point inside the panel. Callers that produce screen
// coordinates without going through the chip mapping, such as injected
// touches, use this so every point ft6336_poll reports obeys the same bounds.
ft_point_t ft6336_clamp_point(int screen_x, int screen_y);
#endif
