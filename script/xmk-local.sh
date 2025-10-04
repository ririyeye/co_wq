#!/bin/bash
set -euo pipefail

BASEDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$BASEDIR/.."

MODE="full"
while (($#)); do
    case "$1" in
        --core)
            MODE="core"
            ;;
        --full)
            MODE="full"
            ;;
        -h|--help)
            cat <<'EOF'
用法：xmk-local.sh [--full|--core]

  --full   启用全部可选模块（网络/TLS/USB）并构建 examples（默认）
  --core   仅构建核心工作队列与同步原语，不启用网络/USB/示例
EOF
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            exit 1
            ;;
    esac
    shift
done

if [[ "$MODE" == "full" ]]; then
    CONFIG_FLAGS=(--USING_NET=y --USING_SSL=y --USE_BUNDLED_LLHTTP=y --USING_USB=y --USING_EXAMPLE=y)
else
    CONFIG_FLAGS=(--USING_NET=n --USING_SSL=n --USE_BUNDLED_LLHTTP=n --USING_USB=n --USING_EXAMPLE=n)
fi

export XMAKE_GLOBALDIR="$(pwd)"
xmake f -y \
    -vD \
    -m releasedbg \
    -o build \
    "${CONFIG_FLAGS[@]}"

xmake project -k compile_commands
xmake -vD
xmake install -o install
