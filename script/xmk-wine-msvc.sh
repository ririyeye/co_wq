#!/bin/bash
set -euo pipefail

BASEDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$BASEDIR/.."

# wine-msvc安装参考
# https://github.com/mstorsjo/msvc-wine
# 编译卡住的时候用wineserver -k 重启wine

SDK_PATH=${SDK_PATH:-/opt/toolchain/msvc-wine/msvc}

MODE="core"
while (($#)); do
    case "$1" in
        --core)
            MODE="core"
            ;;
        --full)
            MODE="full"
            ;;
        --sdk)
            shift
            if [[ $# -eq 0 ]]; then
                echo "--sdk 需要一个参数" >&2
                exit 1
            fi
            SDK_PATH="$1"
            ;;
        --sdk=*)
            SDK_PATH="${1#*=}"
            ;;
        -h|--help)
            cat <<'EOF'
用法：xmk-wine-msvc.sh [--full|--core] [--sdk <path>]

    --core         仅构建核心工作队列与同步原语（默认）
    --full         启用网络/TLS/USB 及示例
  --sdk <path>   指定 msvc-wine SDK 路径（默认 /opt/toolchain/msvc-wine/msvc）
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

if [[ ! -d "$SDK_PATH" ]]; then
    echo "警告：SDK 路径 $SDK_PATH 不存在，后续命令可能失败" >&2
fi

export XMAKE_GLOBALDIR="$(pwd)"

BASE_FLAGS=(
    -y
    --sdk="$SDK_PATH"
    -p windows
    -a x64
    -vD
    -m releasedbg
    -o build
)

if [[ "$MODE" == "full" ]]; then
    CONFIG_FLAGS=(--USING_NET=y --USE_BUNDLED_LLHTTP=y --USING_SSL=y --USING_USB=y --USING_EXAMPLE=y)
else
    CONFIG_FLAGS=(--USING_NET=n --USE_BUNDLED_LLHTTP=n --USING_SSL=n --USING_USB=n --USING_EXAMPLE=n)
fi

# export PATH=/opt/toolchain/msvc-wine/msvc/bin/x64:$PATH
xmake f "${BASE_FLAGS[@]}" "${CONFIG_FLAGS[@]}"

xmake -vD
# 明确构建静态库目标，避免 install 提示未构建
xmake -vD co_wq
xmake install -o install
# 生成 compile_commands 便于 IDE 使用
xmake project -k compile_commands
