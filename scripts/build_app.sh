#!/bin/bash
# Cross-compile the userspace command line tool.
set -euo pipefail

WORK=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SDK=${SDK:-/build/sdk}

make -C "$WORK/apps/gpio3a1_cli" SDK="$SDK"
