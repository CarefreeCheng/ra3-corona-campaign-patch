@echo off
setlocal
chcp 936 >nul
title Red Alert 3 Corona Patch Uninstaller

set "GAME=%~1"

echo ============================================================
echo   红警三 日冕 战役增强补丁  卸载程序
echo ============================================================
echo.

if "%GAME%"=="" if exist "D:\a game\Red Alert 3\Data\ra3_1.12.game" set "GAME=D:\a game\Red Alert 3"
if "%GAME%"=="" (
  echo 请把游戏文件夹拖到本文件上，或者直接输入路径。
  set /p GAME=游戏路径: 
)
if not exist "%GAME%\Data\ra3_1.12.game" (
  echo [错误] 不是日冕安装目录。
  pause
  exit /b 1
)

tasklist /fi "imagename eq ra3_1.12.game" 2>nul | find /i "ra3_1.12.game" >nul
if not errorlevel 1 (
  echo [错误] 游戏还在运行，请先退出。
  pause
  exit /b 1
)

echo [1/3] 还原 xinput1_3.dll ...
if exist "%GAME%\Data\xinput1_3.dll.bak" (
  copy /y "%GAME%\Data\xinput1_3.dll.bak" "%GAME%\Data\xinput1_3.dll" >nul
  del /q "%GAME%\Data\xinput1_3.dll.bak" >nul
  echo        Data 目录已还原
) else (
  echo        Data 目录没有备份，保持原样
)
if exist "%GAME%\xinput1_3.dll.bak" (
  copy /y "%GAME%\xinput1_3.dll.bak" "%GAME%\xinput1_3.dll" >nul
  del /q "%GAME%\xinput1_3.dll.bak" >nul
  echo        根目录已还原
)

echo [2/3] 删除补丁文件 ...
del /q "%GAME%\Data\csprobe.dll" >nul 2>nul
del /q "%GAME%\csprobe.dll" >nul 2>nul
del /q "%GAME%\Data\ra3fix_helper.exe" >nul 2>nul
del /q "%GAME%\ra3fix_helper.exe" >nul 2>nul
del /q "%GAME%\Data\xinput_proxy.cfg" >nul 2>nul
del /q "%GAME%\Data\ra3dump_cs.exe" >nul 2>nul
del /q "%GAME%\ra3dump_cs.exe" >nul 2>nul
echo        完成

echo [3/3] 清理日志 ...
del /q "%GAME%\Data\xinput_proxy.log" >nul 2>nul
del /q "%GAME%\Data\createhook.log" >nul 2>nul
del /q "%GAME%\Data\ra3fix_helper.log" >nul 2>nul
echo        完成

echo.
echo ============================================================
echo   卸载完成。ra3copilot.dll 保留未删（其他工具可能还在用）。
echo   如需彻底清理，可手动删除 %GAME%\Data\ra3copilot.dll
echo ============================================================
pause
exit /b 0