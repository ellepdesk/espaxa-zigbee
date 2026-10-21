#!/bin/sh
# Build and run the host tests for the AXA motion state machine.
# Usage: tests/run.sh   (MOTION_LOG=1 tests/run.sh for the state machine's log)
set -e
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/espaxa-test-motion"
cc -std=c11 -Wall -Wextra -Werror -Wshadow -g -fsanitize=address,undefined \
	-Isrc src/motion.c tests/test_motion.c -o "$out"
"$out"
