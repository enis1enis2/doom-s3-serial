@echo off
setlocal

set "ROOT=%~dp0.."
set "PY=%ROOT%\win-venv\Scripts\python.exe"
set "PORT=%~1"

if "%PORT%"=="" (
    echo Usage: tools\board_info.bat COM3
    exit /b 1
)

if not exist "%PY%" (
    echo Python environment not found. Run tools\flash_all.bat COM3 once first.
    exit /b 1
)

echo === Chip model / features ===
"%PY%" -m esptool --port "%PORT%" chip_id || exit /b 1

echo === Flash chip ===
"%PY%" -m esptool --port "%PORT%" flash_id || exit /b 1

echo === eFuse PSRAM field - embedded PSRAM only ===
"%PY%" -m espefuse --port "%PORT%" summary || exit /b 1
