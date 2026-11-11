#pragma once

#include <gui/canvas.h>
#include "../helpers/subghz_activity.h"

void subghz_receiver_activity_draw(
    Canvas* canvas,
    const SubGhzActivityHistory* history,
    uint8_t top,
    uint8_t height);

void subghz_receiver_activity_draw_fill(Canvas* canvas, int x, int y, int baseline);
void subghz_receiver_activity_draw_stroke(
    Canvas* canvas,
    int x,
    int y,
    int baseline,
    int previous_y);
