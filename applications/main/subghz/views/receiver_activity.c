#include "receiver_activity.h"

static int subghz_receiver_activity_y(int16_t level, int top, int baseline) {
    if(level < -240) level = -240;
    if(level > -60) level = -60;
    return baseline - (level + 240) * (baseline - top) / 180;
}

void subghz_receiver_activity_draw_fill(Canvas* canvas, int x, int y, int baseline) {
    static const uint8_t shade[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for(int fill = y + 1; fill <= baseline; ++fill) {
        const int depth = (fill - y - 1) * 3 / (baseline - y);
        const unsigned density = depth == 0 ? 8 : (depth == 1 ? 4 : 2);
        if(shade[fill & 3][x & 3] < density) canvas_draw_dot(canvas, x, fill);
    }
}

void subghz_receiver_activity_draw_stroke(
    Canvas* canvas,
    int x,
    int y,
    int baseline,
    int previous_y) {
    if(previous_y >= 0) {
        canvas_draw_line(canvas, x - 1, previous_y, x, y);
    } else {
        canvas_draw_dot(canvas, x, y);
    }
    if(y < baseline) canvas_draw_dot(canvas, x, y + 1);
}

void subghz_receiver_activity_draw(
    Canvas* canvas,
    const SubGhzActivityHistory* history,
    uint8_t top,
    uint8_t height) {
    if(height < 4) return;
    const int bottom = top + height - 1;
    const int baseline = bottom - 2;
    const SubGhzActivityBucket* previous = NULL;
    int previous_y = 0;
    for(unsigned x = 0; x < SUBGHZ_ACTIVITY_COLUMNS; ++x) {
        const SubGhzActivityBucket* bucket =
            &history->buckets[(history->next + x) % SUBGHZ_ACTIVITY_COLUMNS];
        canvas_set_color(canvas, ColorBlack);
        if(bucket->flags & SubGhzActivityDecoded) {
            canvas_draw_line(canvas, x, bottom - 1, x, bottom);
        }
        if(!(bucket->flags & SubGhzActivityValid)) {
            previous = NULL;
            continue;
        }
        const int y = subghz_receiver_activity_y(bucket->peak, top, baseline);
        subghz_receiver_activity_draw_fill(canvas, x, y, baseline);
        if(bucket->flags & SubGhzActivityBackground && (x % 4) < 2) {
            const int floor_y = subghz_receiver_activity_y(bucket->background, top, baseline);
            canvas_set_color(canvas, ColorWhite);
            if(floor_y > y + 1) canvas_draw_dot(canvas, x, floor_y - 1);
            if(floor_y < baseline) canvas_draw_dot(canvas, x, floor_y + 1);
            canvas_set_color(canvas, ColorBlack);
            canvas_draw_dot(canvas, x, floor_y);
        }
        const bool joined = previous && previous->context == bucket->context &&
                            !((previous->flags | bucket->flags) & SubGhzActivityBreak) &&
                            (uint32_t)(bucket->at_ms - previous->at_ms) ==
                                SUBGHZ_ACTIVITY_BUCKET_MS;
        subghz_receiver_activity_draw_stroke(canvas, x, y, baseline, joined ? previous_y : -1);
        if(bucket->peak > -60) canvas_draw_dot(canvas, x, top + 1);
        if(bucket->peak < -240) canvas_draw_dot(canvas, x, baseline - 1);
        previous = bucket;
        previous_y = y;
    }
    canvas_set_color(canvas, ColorBlack);
}
