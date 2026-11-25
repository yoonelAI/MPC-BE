@echo off
rem MPC-BE BDMenu 补丁安装：把 files\ 里的文件复制到已安装的 MPC-BE x64，先备份被替换的文件
setlocal EnableDelayedExpansion
net session >nul 2>&1
if errorlevel 1 (
  echo 需要管理员权限，正在请求...
  if "%~1"=="" (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs") else (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '\"%~1\"' -Verb RunAs")
  exit /b
)
set "SRC=%~dp0files"
set "DST=%~1"
if "%DST%"=="" set "DST=%ProgramFiles%\MPC-BE x64"
if not exist "%DST%\mpc-be64.exe" (
  echo [错误] 在 "%DST%" 没找到 mpc-be64.exe
  echo 用法：install_patch.cmd "MPC-BE 安装目录"
  pause & exit /b 1
)
tasklist /fi "imagename eq mpc-be64.exe" | find /i "mpc-be64.exe" >nul && (
  echo [错误] 请先关闭 MPC-BE 播放器
  pause & exit /b 1
)
set "BK=%DST%\BDMenu_backup"
if not exist "%BK%\mpc-be64.exe" (
  echo 备份原文件到 %BK% ...
  mkdir "%BK%" >nul 2>&1
  for %%F in (mpc-be64.exe mpciconlib.dll d3dcompiler_47.dll d3dx9_43.dll) do if exist "%DST%\%%F" copy /y "%DST%\%%F" "%BK%\" >nul
  if exist "%DST%\Lang" xcopy /e /i /y /q "%DST%\Lang" "%BK%\Lang" >nul
  if exist "%DST%\Shaders" xcopy /e /i /y /q "%DST%\Shaders" "%BK%\Shaders" >nul
  if exist "%DST%\Shaders11" xcopy /e /i /y /q "%DST%\Shaders11" "%BK%\Shaders11" >nul
) else (
  echo 已有备份 %BK%（保留最早的原版备份）
)
echo 复制补丁文件 ...
xcopy /e /i /y /q "%SRC%\*" "%DST%\" >nul
if errorlevel 1 (echo [错误] 复制失败 & pause & exit /b 1)
echo.
echo 完成。安装版现在和绿色版功能相同。日志在 %%LOCALAPPDATA%%\MPC-BE-BDMenu\bdnav.log
echo 卸载补丁：运行 uninstall_patch.cmd
pause
