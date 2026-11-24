#!/bin/bash
# Build everything that can be built without a board: .ko + .dtbo + app.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

"$WORK/scripts/build_drv.sh"
"$WORK/scripts/build_dtbo.sh"
"$WORK/scripts/build_app.sh"

echo
echo "Artifacts:"
echo "  $WORK/drivers/gpio3a1/gpio3a1.ko"
echo "  $WORK/dts/gpio3a1.dtbo"
echo "  $WORK/apps/gpio3a1_cli/gpio3a1_cli"
