@echo off
cd /d "%~dp0"
set "LOG=%~dp0bdprobe_log.txt"
set "DISC=%~1"
if "%DISC%"=="" set "DISC=C:\迅雷下载\Vita.and.Virginia.2018.1080p.BluRay.AVC.DTS-HD.MA.5.1-OCULAR"
echo 正在测试: %DISC%
echo 请稍候，最多约 90 秒...
bdprobe.exe "%DISC%" "%~dp0out" 2 > "%LOG%" 2>&1
echo exitcode=%ERRORLEVEL%>> "%LOG%"
type "%LOG%"
echo.
echo 完成，请回到聊天告诉我
pause
