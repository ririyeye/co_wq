
@echo off
setlocal

pushd "%~dp0.."
set "XMAKE_GLOBALDIR=%CD%"
echo %XMAKE_GLOBALDIR%

xmake.exe f -y -vD --USING_EXAMPLE=y -m releasedbg -o build -P .
xmake.exe project -k compile_commands -P .
xmake.exe -vD -P .
xmake.exe install -o install -P .

popd
endlocal
