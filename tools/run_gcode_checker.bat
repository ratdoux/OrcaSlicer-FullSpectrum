@echo off
REM G-code边界检查工具 - Windows启动脚本

echo ========================================
echo G-code边界超限检查工具
echo ========================================
echo.

REM 检查Python是否安装
python --version >nul 2>&1
if errorlevel 1 (
    echo 错误: 未找到Python
    echo 请先安装Python 3.7或更高版本
    echo 下载地址: https://www.python.org/downloads/
    pause
    exit /b 1
)

echo 启动GUI界面...
echo.

python "%~dp0gcode_boundary_checker_gui.py"

if errorlevel 1 (
    echo.
    echo 程序运行出错，按任意键退出...
    pause
)
