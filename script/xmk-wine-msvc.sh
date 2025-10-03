#!/bin/bash
BASEDIR=$(dirname "$0")
cd $BASEDIR/..

# wine-msvc安装参考
# https://github.com/mstorsjo/msvc-wine
# 编译卡住的时候用wineserver -k 重启wine

export XMAKE_GLOBALDIR=$(pwd)

# export PATH=/opt/toolchain/msvc-wine/msvc/bin/x64:$PATH
xmake f -y \
    --sdk=/opt/toolchain/msvc-wine/msvc \
    -p windows -a x64 \
    --USING_EXAMPLE=y \
    -vD \
    -m releasedbg \
    -o build
    
xmake -vD
# 明确构建静态库目标，避免 install 提示未构建
xmake -vD co_wq
xmake install -o install
# 生成 compile_commands 便于 IDE 使用
xmake project -k compile_commands
