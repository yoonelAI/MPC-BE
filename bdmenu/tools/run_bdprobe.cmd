@echo off
chcp 65001 >nul
set "DISC=%~1"
if "%DISC%"=="" set "DISC=C:\迅雷下载\Vita.and.Virginia.2018.1080p.BluRay.AVC.DTS-HD.MA.5.1-OCULAR"
echo 正在测试: %DISC%
"%~dp0bdprobe.exe" "%DISC%" "%~dp0out" 2 > "%~dp0bdprobe_log.txt" 2>&1
type "%~dp0bdprobe_log.txt"
echo.
echo 完成，结果在 bdprobe_log.txt 和 out 文件夹
pause
