/*
 * NiMH state of charge from voltage, see battery_level.h
 *
 * Rest voltage per cell at light load (the XIAO, AXA idle). Fresh off the
 * charger a cell reads up to about 1.45 V, which settles within a day; under
 * 1.0 V it is empty. Points in between follow a typical NiMH discharge curve
 * at a low rate. Calibrate against the real pack (TESTING.md).
 */

#include "battery_level.h"

static const struct {
	int cell_mv;
	uint8_t percent;
} curve[] = {
	{1380, 100},
	{1300, 90},
	{1260, 75},
	{1230, 55},
	{1200, 35},
	{1160, 20},
	{1120, 10},
	{1080, 5},
	{1000, 0},
};

#define CURVE_POINTS (sizeof(curve) / sizeof(curve[0]))

uint8_t battery_level_percent(int cell_mv)
{
	if (cell_mv >= curve[0].cell_mv) {
		return curve[0].percent;
	}
	for (unsigned int i = 1; i < CURVE_POINTS; i++) {
		if (cell_mv >= curve[i].cell_mv) {
			int span_mv = curve[i - 1].cell_mv - curve[i].cell_mv;
			int span_pct = curve[i - 1].percent - curve[i].percent;
			int above = cell_mv - curve[i].cell_mv;

			/* Linear between the two points, rounded */
			return (uint8_t)(curve[i].percent + (above * span_pct + span_mv / 2) / span_mv);
		}
	}
	return 0;
}
