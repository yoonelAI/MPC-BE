@echo off
cd /d "%~dp0"
set "LOG=%~dp0bdprobe_log.txt"
set "DISC=%~1"
if "%DISC%"=="" set "DISC=E:\AI\MPC-BE-BDMenu\KARA 2nd JAPAN TOUR 2013 KARASIA Disc1.iso"
set "REGION=%~2"
if "%REGION%"=="" set "REGION=1"
set "RUN=%~dp0run"
if not exist "%RUN%" mkdir "%RUN%"
copy /y "%~dp0bdprobe.exe" "%RUN%\bdprobe.exe" >nul
if exist "%~dp0out" rd /s /q "%~dp0out"
echo 正在测试: %DISC%  区码掩码=%REGION%
echo 请稍候，约 1-2 分钟...
"%RUN%\bdprobe.exe" "%DISC%" "%~dp0out" %REGION% > "%LOG%" 2>&1
echo exitcode=%ERRORLEVEL%>> "%LOG%"
findstr /b "SUMMARY RESULT [nav] [mouse] [key]" "%LOG%"
echo.
echo 完成，请回到聊天告诉我
pause
