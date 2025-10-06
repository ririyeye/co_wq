@echo off
setlocal enabledelayedexpansion

if "%~1"=="" (
    echo Usage: %~nx0 ^<interface-id^> [duration-seconds]
    echo.
    echo   interface-id  Value from "dumpcap -D" or Wireshark interface list.
    echo   duration      Optional capture length in seconds (default: 120).
    exit /b 1
)

set IFACE=%~1
set DURATION=%~2
if not defined DURATION set DURATION=120

set TARGET_HOST=www.baidu.com
set SCRIPT_DIR=%~dp0
set LOG_DIR=%SCRIPT_DIR%..\logs
if not exist "%LOG_DIR%" (
    mkdir "%LOG_DIR%"
)

for /f "usebackq tokens=* delims=" %%i in (`powershell -NoLogo -NoProfile -Command "(Get-Date).ToString('yyyyMMdd_HHmmss')"`) do set TIMESTAMP=%%i
set OUTPUT=%LOG_DIR%\proxy_capture_!TIMESTAMP!.pcapng

set DUMPCAP=
for /f "usebackq delims=" %%i in (`where dumpcap 2^>nul`) do (
    set DUMPCAP=%%i
    goto :found_dumpcap
)
set WIRESHARK_DEFAULT=C:\Program Files\Wireshark\dumpcap.exe
if exist "%WIRESHARK_DEFAULT%" (
    set DUMPCAP=%WIRESHARK_DEFAULT%
    goto :found_dumpcap
)
echo [capture] dumpcap.exe not found. Ensure Wireshark is installed or add it to PATH.
exit /b 2

:found_dumpcap

set BASE_FILTER=tcp port 18100
set FILTER=%BASE_FILTER%
for /f "usebackq tokens=* delims=" %%i in (`powershell -NoLogo -NoProfile -Command "$host='%TARGET_HOST%'; try { $ips = Resolve-DnsName -Name $host -Type A -ErrorAction Stop ^| Select-Object -ExpandProperty IPAddress } catch { $ips = @() }; if ($ips -and $ips.Count -gt 0) { $conditions = $ips ^| ForEach-Object { 'host ' + $_ }; 'tcp port 18100 or (tcp port 443 and (' + ($conditions -join ' or ') + '))' } else { 'tcp port 18100' }"`) do set FILTER=%%i

if /i "%FILTER%"=="%BASE_FILTER%" (
    echo [capture] warning: failed to resolve %TARGET_HOST%. Capturing only on local port 18100.
)

echo [capture] Using filter: !FILTER!
echo [capture] Writing to: !OUTPUT!
"%DUMPCAP%" -i %IFACE% -a duration:%DURATION% -f "!FILTER!" -w "!OUTPUT!"
set EXITCODE=%ERRORLEVEL%
if %EXITCODE% EQU 0 (
    echo [capture] capture complete.
) else (
    echo [capture] dumpcap exited with code %EXITCODE%.
)
exit /b %EXITCODE%
