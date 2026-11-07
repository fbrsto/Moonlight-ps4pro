/* Holds presented frames to a regular schedule at the chosen frame rate.
 *
 * Frames that come early wait for their slot, which evens out network jitter.
 * A frame more than half a slot early is dropped: that caps the rate when the PC
 * sends faster than asked, and keeps the wait, so the latency, bounded. */
#ifndef FRAME_PACER_H
#define FRAME_PACER_H

#include <stdint.h>

#define PACER_RECOVERY_DIV 16

typedef struct {
    uint64_t interval_us; /* 0 = pacing off */
    uint64_t due_us;      /* next slot; 0 = start from the next frame */
} frame_pacer_t;

static inline void frame_pacer_set(frame_pacer_t *p, int fps) {
    p->interval_us = fps > 0 ? 1000000u / (unsigned)fps : 0;
    p->due_us = 0;
}

static inline int frame_pacer_should_skip(const frame_pacer_t *p, uint64_t now_us) {
    return p->interval_us && p->due_us > now_us &&
           p->due_us - now_us > p->interval_us / 2;
}

/* Takes the next slot; returns how long to wait before showing the frame. */
static inline uint64_t frame_pacer_next_wait(frame_pacer_t *p, uint64_t now_us) {
    if (!p->interval_us)
        return 0;
    /* First frame, or a late one: the schedule moves back to the latest arrivals,
     * so that the frames around them have room to wait and come out evenly. */
    if (p->due_us <= now_us) {
        p->due_us = now_us + p->interval_us;
        return 0;
    }
    /* Early: wait, and creep back so one late spike does not cost latency for good. */
    uint64_t wait = p->due_us - now_us;
    p->due_us += p->interval_us - wait / PACER_RECOVERY_DIV;
    return wait;
}

#endif
