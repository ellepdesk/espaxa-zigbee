"""Checks that harness.py agrees with the compiled C code."""

import pytest

from harness import Cmd, Code, Lock, Phase, enum_value


@pytest.mark.parametrize(
    "prefix, enum",
    [("PHASE_", Phase), ("LOCK_", Lock), ("CMD_", Cmd), ("CODE_", Code)],
)
def test_enum_copies_match_motion_h(prefix, enum):
    for member in enum:
        assert enum_value(prefix + member.name) == member.value, member
