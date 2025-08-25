#!/usr/bin/env bash

set -euo pipefail

IMAGE_NAME="sotp:dev"

docker build -t "$IMAGE_NAME" "$(cd "$(dirname "$0")" && pwd)"

docker run -it --rm \
  --name so-tp1 \
  -v "$(cd "$(dirname "$0")" && pwd)":/sotp \
  -w /sotp \
  "$IMAGE_NAME" \
  /bin/bash


