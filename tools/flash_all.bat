@echo off
setlocal

set "ROOT=%~dp0.."
set "VENV=%ROOT%\win-venv"
set "PY=%VENV%\Scripts\python.exe"
set "PORT=%~1"

if "%PORT%"=="" (
    echo Usage: tools\flash_all.bat COM3
    exit /b 1
)

if not exist "%PY%" (
    echo Creating local Python environment: %VENV%
    where py >nul 2>nul
    if not errorlevel 1 (
        py -m venv "%VENV%" || exit /b 1
    ) else (
        python -m venv "%VENV%" || exit /b 1
    )
)

"%PY%" -c "import esptool, serial" >nul 2>nul
if errorlevel 1 (
    echo Installing esptool and pyserial into %VENV%
    "%PY%" -m pip install esptool pyserial || exit /b 1
)

for %%F in (
    "%ROOT%\build\bootloader\bootloader.bin"
    "%ROOT%\build\partition_table\partition-table.bin"
    "%ROOT%\build\doom_s3_serial.bin"
    "%ROOT%\doom1.wad"
) do (
    if not exist %%F (
        echo Missing %%F
        exit /b 1
    )
)

echo Flashing firmware + WAD to %PORT%
"%PY%" -m esptool --chip esp32s3 --port "%PORT%" --baud 460800 write_flash --flash_size 8MB 0x0 "%ROOT%\build\bootloader\bootloader.bin" 0x8000 "%ROOT%\build\partition_table\partition-table.bin" 0x10000 "%ROOT%\build\doom_s3_serial.bin" 0x190000 "%ROOT%\doom1.wad"
