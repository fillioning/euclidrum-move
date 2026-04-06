#!/usr/bin/env bash
# Build Euclidrum module for Schwung (ARM64)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
IMAGE_NAME="schwung-builder"
MODULE_ID="euclidrum"

if [ -z "$CROSS_PREFIX" ] && command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    CROSS_PREFIX="aarch64-linux-gnu-"
fi

if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    if ! command -v docker >/dev/null 2>&1; then
        echo "Error: docker not found and no local cross-compiler detected."
        exit 1
    fi

    echo "=== Euclidrum Module Build (via Docker) ==="
    echo ""

    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$REPO_ROOT/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    echo "Running build in Docker..."
    WIN_ROOT="$(cd "$REPO_ROOT" && pwd -W 2>/dev/null || pwd)"
    SCHWUNG_ROOT="$(cd "$REPO_ROOT/../schwung/src" && pwd -W 2>/dev/null || echo "$REPO_ROOT/../schwung/src")"

    CONTAINER_ID=$(MSYS_NO_PATHCONV=1 docker create \
        -w /build \
        "$IMAGE_NAME" \
        bash -c "dos2unix build-module.sh 2>/dev/null; SCHWUNG_SRC=/schwung-src bash build-module.sh")

    docker cp "$WIN_ROOT/euclidrum.c" "$CONTAINER_ID:/build/euclidrum.c"
    docker cp "$WIN_ROOT/module.json" "$CONTAINER_ID:/build/module.json"
    docker cp "$WIN_ROOT/build-module.sh" "$CONTAINER_ID:/build/build-module.sh"
    docker cp "$SCHWUNG_ROOT" "$CONTAINER_ID:/schwung-src"

    docker start -a "$CONTAINER_ID"

    mkdir -p "$REPO_ROOT/dist/$MODULE_ID"
    docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/dsp.so" "$REPO_ROOT/dist/$MODULE_ID/dsp.so"
    docker cp "$CONTAINER_ID:/build/dist/$MODULE_ID/module.json" "$REPO_ROOT/dist/$MODULE_ID/module.json"
    docker rm "$CONTAINER_ID" > /dev/null

    echo ""
    echo "=== Done ==="
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
cd "$REPO_ROOT"

echo "=== Building Euclidrum Module ==="
echo "Cross prefix: $CROSS_PREFIX"

SCHWUNG_SRC="${SCHWUNG_SRC:-$REPO_ROOT/../schwung/src}"
if [ ! -d "$SCHWUNG_SRC/host" ]; then
    SCHWUNG_SRC="$REPO_ROOT/../schwung/src"
fi
if [ ! -d "$SCHWUNG_SRC/host" ]; then
    echo "Error: host headers not found."
    echo "Clone schwung or schwung next to this repo, or set SCHWUNG_SRC."
    exit 1
fi

echo "Host headers: $SCHWUNG_SRC"

mkdir -p build
mkdir -p dist/euclidrum

echo "Compiling dsp.so..."
${CROSS_PREFIX}gcc -O2 -fPIC -ffast-math -shared \
    -I "$SCHWUNG_SRC" \
    euclidrum.c \
    -o build/dsp.so \
    -lm

echo "Packaging..."
cp module.json dist/euclidrum/module.json
cp build/dsp.so dist/euclidrum/dsp.so
chmod +x dist/euclidrum/dsp.so

cd dist
tar -czvf euclidrum-module.tar.gz euclidrum/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/euclidrum/"
echo "Tarball: dist/euclidrum-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./install.sh"
