#include "test_util.h"
#include "input/ft6336_map.h"

#define SCREEN_W 480
#define SCREEN_H 320

int main(void) {
    // chip Y becomes screen X; chip X becomes screen Y, inverted.
    ft_point_t p = ft6336_map_point(0, 0);
    ASSERT_EQ(p.x, 0);
    ASSERT_EQ(p.y, SCREEN_H - 1);

    p = ft6336_map_point(SCREEN_H - 1, SCREEN_W - 1);
    ASSERT_EQ(p.x, SCREEN_W - 1);
    ASSERT_EQ(p.y, 0);

    p = ft6336_map_point(160, 240);
    ASSERT_EQ(p.x, 240);
    ASSERT_EQ(p.y, SCREEN_H - 1 - 160);

    // A chip that reports past the panel, or a negative after inversion, is
    // still a point on the panel.
    p = ft6336_map_point(-50, -50);
    ASSERT_EQ(p.x, 0);
    ASSERT_EQ(p.y, SCREEN_H - 1);

    p = ft6336_map_point(9999, 9999);
    ASSERT_EQ(p.x, SCREEN_W - 1);
    ASSERT_EQ(p.y, 0);

    // The clamp on its own, which is what injected points go through.
    p = ft6336_clamp_point(0, 0);
    ASSERT_EQ(p.x, 0);
    ASSERT_EQ(p.y, 0);

    p = ft6336_clamp_point(SCREEN_W - 1, SCREEN_H - 1);
    ASSERT_EQ(p.x, SCREEN_W - 1);
    ASSERT_EQ(p.y, SCREEN_H - 1);

    p = ft6336_clamp_point(SCREEN_W, SCREEN_H);
    ASSERT_EQ(p.x, SCREEN_W - 1);
    ASSERT_EQ(p.y, SCREEN_H - 1);

    // The agentio TCH command parses into uint16_t, so this is the worst a
    // host can inject.
    p = ft6336_clamp_point(65535, 65535);
    ASSERT_EQ(p.x, SCREEN_W - 1);
    ASSERT_EQ(p.y, SCREEN_H - 1);

    p = ft6336_clamp_point(-1, -1);
    ASSERT_EQ(p.x, 0);
    ASSERT_EQ(p.y, 0);

    TEST_RETURN();
}
