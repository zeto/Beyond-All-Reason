#!/bin/bash
# Fail the build when bar-metal is not a native arm64 Metal binary.
set -euo pipefail

bin="${1:?binary path required}"
if [[ ! -f "$bin" ]]; then
  echo "linkage: missing $bin" >&2
  exit 1
fi

libs="$(otool -L "$bin")"
archs="$(lipo -archs "$bin")"
printf '%s\n' "$libs"
echo "archs: $archs"

if [[ "$archs" != "arm64" ]]; then
  echo "linkage: expected arm64 only, got: $archs" >&2
  exit 1
fi

if printf '%s\n' "$libs" | grep -Eiq 'OpenGL|libGL|Vulkan|MoltenVK|Mesa|zink|kosmickrisp|[Ww]ine|SDL'; then
  echo "linkage: translation layer or foreign graphics API is linked" >&2
  exit 1
fi
