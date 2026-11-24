#!/bin/bash
# Build the out-of-tree kernel module.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}

make -C "$WORK/drivers/gpio3a1" SDK="$SDK"
