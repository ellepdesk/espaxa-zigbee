/*
 * AXA Remote 2.0 driver
 *
 * Talks to the AXA over its serial line and estimates the window position
 * from travel time. Runs in its own thread, the functions below only queue a
 * request and are safe to call from the Zigbee thread.
 */

#ifndef AXA_H_
#define AXA_H_

#include <stdint.h>

/*
 * Called from the AXA thread whenever the position changes, 0 = closed,
 * 100 = open. Returns 0 when the report was accepted; on an error the same
 * position is offered again later.
 */
typedef int (*axa_position_cb_t)(uint8_t open_percent);

int axa_init(axa_position_cb_t position_cb);

/* Move to a position, 0 = closed (and locked), 100 = fully open. */
void axa_move_to(uint8_t open_percent);
void axa_stop(void);

#endif /* AXA_H_ */
