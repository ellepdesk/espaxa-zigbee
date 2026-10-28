import pytest

from harness import Axa, Code, build


@pytest.fixture(scope="session", autouse=True)
def built_library():
    """Compile src/motion.c and src/battery_level.c with the shim once per run."""
    return build()


@pytest.fixture
def axa():
    """Booted closed and locked, as confirmed by the first STATUS; logs empty."""
    a = Axa()
    a.start(status=Code.LOCKED)
    a.run(1000)
    a.clear_log()
    return a


@pytest.fixture
def axa_open(axa):
    """Opened fully from boot and settled; the AXA reports unlocked."""
    axa.status = Code.UNLOCKED
    axa.move_to(100)
    axa.run(70_000)
    axa.clear_log()
    return axa


@pytest.fixture
def axa_locking(axa_open):
    """Closing from fully open until the estimate reached closed (locking)."""
    axa_open.move_to(0)
    axa_open.run(52_000)
    axa_open.clear_log()
    return axa_open
