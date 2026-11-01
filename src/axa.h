/*
 * AXA Remote 2.0 driver
 *
 * Talks to the AXA over its serial line and estimates the window position
 * from travel time. Runs in its own thread, the functions below only queue a
 * request and are safe to call from the Zigbee thread.
 */

#ifndef AXA_H_
#define AXA_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Called from the AXA thread whenever the position changes, 0 = closed,
 * 100 = open. Returns 0 when the report was accepted; on an error the same
 * position is offered again later.
 */
typedef int (*axa_position_cb_t)(uint8_t open_percent);

/*
 * Called from the AXA thread after each STATUS reply: true only for 211
 * Strong Locked, false for unlocked and for 212 (closed, not fully locked).
 */
typedef void (*axa_lock_cb_t)(bool strong_locked);

int axa_init(axa_position_cb_t position_cb, axa_lock_cb_t lock_cb);

/* Move to a position, 0 = closed (and locked), 100 = fully open. */
void axa_move_to(uint8_t open_percent);
void axa_stop(void);

/*
 * Time since the AXA was last busy (a serial exchange or a move), 0 while it
 * is busy and until the boot exchange is over. Safe from any thread.
 */
uint32_t axa_quiet_ms(void);

#endif /* AXA_H_ */
