"""Tests for the NiMH voltage to percentage curve (src/battery_level.c)."""

import pytest

from harness import battery_level_percent


@pytest.mark.parametrize(
    "cell_mv, percent",
    [
        # Ends of the curve, and clamping beyond them
        (1450, 100),
        (1380, 100),
        (1000, 0),
        (900, 0),
        (0, 0),
        (-5, 0),
        # On the points
        (1300, 90),
        (1200, 35),
        (1080, 5),
        # Linear in between: halfway 1230 (55) and 1200 (35)
        (1215, 45),
    ],
)
def test_percent_at(cell_mv, percent):
    assert battery_level_percent(cell_mv) == percent


def test_never_increases_as_the_voltage_drops():
    previous = 101
    for mv in range(1500, 899, -1):
        percent = battery_level_percent(mv)
        assert 0 <= percent <= 100
        assert percent <= previous, f"{mv} mV"
        previous = percent
