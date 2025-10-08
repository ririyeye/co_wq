
@echo off
setlocal

pushd "%~dp0.."
set "XMAKE_GLOBALDIR=%CD%"
echo %XMAKE_GLOBALDIR%

set "MODE=full"

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--core" (
	set "MODE=core"
) else if /I "%~1"=="--full" (
	set "MODE=full"
) else if /I "%~1"=="-h" (
	goto :show_help
) else if /I "%~1"=="--help" (
	goto :show_help
) else (
	set "EXIT_CODE=1"
	set "EXIT_MSG=Unknown option: %1"
	goto :cleanup
)
shift
goto parse_args

:show_help
echo Usage: xmsvc.bat [--full^|--core]
echo.
echo   --full   Enable all optional modules and build examples ^(default^)
echo   --core   Build only the core workqueue/sync components
set "EXIT_CODE=0"
goto :cleanup

:args_done

if /I "%MODE%"=="full" (
	set "CONFIG_FLAGS=--USING_NET=y --USE_BUNDLED_LLHTTP=y --USING_SSL=y --USING_USB=y --MSVC_ITERATOR_DEBUG=y --USING_EXAMPLE=y"
) else (
	set "CONFIG_FLAGS=--USING_NET=n --USE_BUNDLED_LLHTTP=n --USING_SSL=n --USING_USB=n --MSVC_ITERATOR_DEBUG=n --USING_EXAMPLE=n"
)

xmake.exe f -y -vD %CONFIG_FLAGS% -m releasedbg -o build -P .
xmake.exe project -k compile_commands -P .
xmake.exe -vD -P .
xmake.exe install -o install -P .

set "EXIT_CODE=%ERRORLEVEL%"

:cleanup
if defined EXIT_MSG echo %EXIT_MSG%
popd
endlocal
exit /b %EXIT_CODE%
