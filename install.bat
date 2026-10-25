@echo off
setlocal
chcp 936 >nul
title Red Alert 3 Corona Patch Installer

set "PKGDIR=%~dp0"
set "GAME=%~1"

echo ============================================================
echo   红警三 日冕 战役增强补丁  v1.0  安装程序
echo ============================================================
echo.

if "%GAME%"=="" if exist "D:\a game\Red Alert 3\Data\ra3_1.12.game" set "GAME=D:\a game\Red Alert 3"
if "%GAME%"=="" (
  echo 请把游戏文件夹拖到本文件上，或者直接输入路径。
  echo 例如：D:\a game\Red Alert 3
  set /p GAME=游戏路径: 
)
if not exist "%GAME%\Data\ra3_1.12.game" (
  echo.
  echo [错误] 这个目录里没有 Data\ra3_1.12.game
  echo        请确认是红警三日冕的安装根目录。
  echo.
  pause
  exit /b 1
)
echo [信息] 游戏目录: %GAME%
echo.

echo [1/5] 检查游戏是否正在运行 ...
tasklist /fi "imagename eq ra3_1.12.game" 2>nul | find /i "ra3_1.12.game" >nul
if not errorlevel 1 (
  echo.
  echo [错误] 游戏还在运行，请先完全退出日冕（含启动器）再安装。
  echo.
  pause
  exit /b 1
)
echo        未检测到游戏进程，可以继续。

echo [2/5] 备份原始文件 ...
if not exist "%GAME%\Data\xinput1_3.dll.bak" copy /y "%GAME%\Data\xinput1_3.dll" "%GAME%\Data\xinput1_3.dll.bak" >nul 2>nul
if not exist "%GAME%\xinput1_3.dll.bak" copy /y "%GAME%\xinput1_3.dll" "%GAME%\xinput1_3.dll.bak" >nul 2>nul
echo        完成（已有备份则跳过）。

echo [3/5] 准备 Ra3-Copilot 核心 ...
if exist "%GAME%\Data\ra3copilot.dll" (
  echo        已存在，跳过下载。
  goto copilot_ok
)
if exist "%PKGDIR%bin\ra3copilot.dll" (
  copy /y "%PKGDIR%bin\ra3copilot.dll" "%GAME%\Data\ra3copilot.dll" >nul
  echo        从安装包复制完成。
  goto copilot_ok
)
echo        正在从 GitHub 下载 Ra3-Copilot v1.2.0 ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/ProbiusOfficial/Ra3-Copilot/releases/download/v1.2.0/Ra3Copilot-v1.2.0-win-x86.zip' -OutFile ($env:TEMP + '\ra3c.zip'); Expand-Archive -LiteralPath ($env:TEMP + '\ra3c.zip') -DestinationPath ($env:TEMP + '\ra3c') -Force; exit 0 } catch { exit 1 }"
if errorlevel 1 goto nopilot
copy /y "%TEMP%\ra3c\ra3copilot.dll" "%GAME%\Data\ra3copilot.dll" >nul
echo        下载完成。

:copilot_ok
echo [4/5] 安装补丁文件 ...
copy /y "%PKGDIR%bin\xinput1_3.dll" "%GAME%\Data\xinput1_3.dll" >nul
copy /y "%PKGDIR%bin\xinput1_3.dll" "%GAME%\xinput1_3.dll" >nul
copy /y "%PKGDIR%bin\csprobe.dll" "%GAME%\Data\csprobe.dll" >nul
copy /y "%PKGDIR%bin\csprobe.dll" "%GAME%\csprobe.dll" >nul
copy /y "%PKGDIR%bin\ra3fix_helper.exe" "%GAME%\Data\ra3fix_helper.exe" >nul
copy /y "%PKGDIR%bin\ra3fix_helper.exe" "%GAME%\ra3fix_helper.exe" >nul
if exist "%PKGDIR%bin\xinput_proxy.cfg" copy /y "%PKGDIR%bin\xinput_proxy.cfg" "%GAME%\Data\xinput_proxy.cfg" >nul
echo        完成。

echo [5/5] 清理旧日志 ...
del /q "%GAME%\Data\xinput_proxy.log" >nul 2>nul
del /q "%GAME%\Data\createhook.log" >nul 2>nul
del /q "%GAME%\Data\ra3fix_helper.log" >nul 2>nul
del /q "%GAME%\Data\dsound.dll" >nul 2>nul
if exist "%GAME%\Data\dsound.dll.bak" copy /y "%GAME%\Data\dsound.dll.bak" "%GAME%\Data\dsound.dll" >nul 2>nul

echo.
echo ============================================================
echo   安装完成
echo ============================================================
echo   1. 启动日冕，进入任意战役关卡
echo   2. 进入后等 20 秒左右（自动注入 + 打补丁）
echo   3. 新造的建筑一定有扩展技能；
echo      地图自带的老建筑需要"重新载入关卡"才会变成扩展版
echo.
echo   日志位置（出问题时把这三个发出来）：
echo     %GAME%\Data\xinput_proxy.log
echo     %GAME%\Data\ra3fix_helper.log
echo     %GAME%\Data\createhook.log
echo.
echo   卸载：双击 uninstall.bat
echo ============================================================
pause
exit /b 0

:nopilot
echo.
echo [警告] 自动下载 Ra3-Copilot 失败（网络问题）。
echo        请手动下载并放到 %GAME%\Data\ra3copilot.dll
echo        下载地址：https://github.com/ProbiusOfficial/Ra3-Copilot/releases/tag/v1.2.0
echo        下完之后重新运行本安装程序。
echo.
pause
exit /b 1