@echo off
setlocal
pushd "%~dp0"
if exist "disc\game.cue" (
    "%~dp0q2psx.exe" --disc "%~dp0disc\game.cue" %*
) else (
    "%~dp0q2psx.exe" %*
)
set "q2psx_exit=%errorlevel%"
popd
exit /b %q2psx_exit%
