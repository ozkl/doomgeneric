#!/usr/bin/env bash
#
# Build the PlayStation 2 port of doomgeneric inside the ps2dev Docker
# toolchain (see Dockerfile). The port itself lives in ps2/.
#
# Usage:
#   ./build.sh                  # build ps2/doomgeneric.elf  (no WAD baked in;
#                               #   supply a WAD at runtime, e.g. via hostfs)
#   ./build.sh EMBED_WAD=1      # also embed ps2/DOOM1.WAD (shareware) as a
#                               #   built-in fallback IWAD -- for convenience
#   ./build.sh clean            # remove build artifacts
#   ./build.sh shell            # interactive shell inside the toolchain (cwd=ps2/)
#
# Artifacts (objects in ps2/build/, the ELF as ps2/doomgeneric.elf) are owned
# by your host user, not root.
set -euo pipefail

IMAGE="ps2dock:local"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Build the local image (ps2dev + make/bash) on first use.
if ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
  echo ">> building ${IMAGE} ..."
  docker build -t "${IMAGE}" "${HERE}"
fi

# Mount the repo at /work and run in ps2/ as the host user.
common=(--rm -u "$(id -u):$(id -g)" -v "${HERE}:/work" -w /work/ps2)

if [[ "${1:-}" == "shell" ]]; then
  exec docker run -it "${common[@]}" "${IMAGE}" /bin/bash
fi

# Everything else is passed straight to make (default target = doomgeneric.elf).
exec docker run "${common[@]}" "${IMAGE}" make "$@"
