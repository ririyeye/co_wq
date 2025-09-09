#!/bin/bash
BASEDIR=$(dirname "$0")
cd $BASEDIR/..

export XMAKE_GLOBALDIR=$(pwd)

xmake g --network=private
xmake f -y \
    -vD \
    --USING_EXAMPLE=y \
    -m releasedbg

xmake project -k compile_commands
xmake -vD
xmake install -o install
