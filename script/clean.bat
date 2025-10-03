@echo off
setlocal

pushd "%~dp0.."
echo Cleaning workspace at %CD%

for %%D in (.xmake build install .cache) do (
	if exist "%%D" (
		echo Removing %%D
		rd /s /q "%%D"
	)
)

if defined USERPROFILE if exist "%USERPROFILE%\.xmake" (
	echo Removing %USERPROFILE%\.xmake
	rd /s /q "%USERPROFILE%\.xmake"
)

popd
endlocal

