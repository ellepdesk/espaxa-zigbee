/*
 * NiMH state of charge from voltage
 *
 * Pure C without Zephyr dependencies, tested on the host (tests/).
 */

#ifndef BATTERY_LEVEL_H_
#define BATTERY_LEVEL_H_

#include <stdint.h>

/* Below this per-cell voltage the reading is treated as "no divider fitted" */
#define BATTERY_LEVEL_MIN_VALID_CELL_MV 500

/*
 * Remaining charge in percent (0-100) for a NiMH cell at rest, from its
 * voltage in mV. Approximate: NiMH has a flat discharge curve, so the middle
 * of the range is only good to about +-15%.
 */
uint8_t battery_level_percent(int cell_mv);

#endif /* BATTERY_LEVEL_H_ */
