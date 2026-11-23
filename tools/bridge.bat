@echo off
setlocal

set "ROOT=%~dp0.."
set "PY=%ROOT%\win-venv\Scripts\python.exe"

if not exist "%PY%" (
    echo Python environment not found. Run tools\flash_all.bat COM3 once first.
    exit /b 1
)

"%PY%" "%ROOT%\tools\bridge.py" %*
