@echo off
rem MPC-BE BDMenu 补丁卸载：从 BDMenu_backup 恢复原文件并删除 bdnav.dll
setlocal
net session >nul 2>&1
if errorlevel 1 (
  if "%~1"=="" (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs") else (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '\"%~1\"' -Verb RunAs")
  exit /b
)
set "DST=%~1"
if "%DST%"=="" set "DST=%ProgramFiles%\MPC-BE x64"
set "BK=%DST%\BDMenu_backup"
tasklist /fi "imagename eq mpc-be64.exe" | find /i "mpc-be64.exe" >nul && (
  echo [错误] 请先关闭 MPC-BE 播放器
  pause & exit /b 1
)
if not exist "%BK%\mpc-be64.exe" (
  echo 没有找到备份，只删除 bdnav.dll（播放器即恢复原版行为）
  del /q "%DST%\bdnav.dll" "%DST%\BDMenu-README.txt" >nul 2>&1
  pause & exit /b 0
)
xcopy /e /i /y /q "%BK%\*" "%DST%\" >nul
del /q "%DST%\bdnav.dll" "%DST%\BDMenu-README.txt" >nul 2>&1
echo 已恢复原版文件。
pause
