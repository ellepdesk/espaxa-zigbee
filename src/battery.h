/*
 * Battery voltage measurement
 *
 * Reads the NiMH pack through the voltage divider on D0 (devicetree node
 * vbatt) every CONFIG_ESPAXA_BATTERY_INTERVAL_MS, but only once the AXA has
 * been quiet for a while: the pack is shared with the motor and sags while
 * it runs.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

/* Called from the system work queue with the pack voltage in mV. */
typedef void (*battery_cb_t)(int pack_mv);

int battery_init(battery_cb_t cb);

#endif /* BATTERY_H_ */
