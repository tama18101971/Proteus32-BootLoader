@echo off
setlocal EnableDelayedExpansion
setlocal EnableExtensions
REM ============================================================================
REM  Proteus32 CH32V003 UART Bootloader - reproducible build (TZ section 6.2)
REM
REM  Usage:  build.cmd        - compile + link + size gate
REM          build.cmd clean  - remove build artifacts
REM
REM  Artifacts:  build\bootloader.elf   (debug)
REM              build\bootloader.bin   (raw image for the boot area, 0x00000000)
REM              build\bootloader.map
REM
REM  Size gate:  hard limit 1920 B (boot-area boundary), target <= 1888 B.
REM              The linker script itself also fails on overflow (LENGTH=1920).
REM
REM  Toolchain:  riscv-wch-elf / riscv-none-embed GCC 12+
REM              discovered via %RISCV_GCC_BIN%, MounRiver Studio installs,
REM              or PATH. Override with: set RISCV_GCC_BIN=C:\path\to\bin
REM ============================================================================

set "ROOT=%~dp0"
set "BLD=%ROOT%build"

if /i "%~1"=="clean" (
    if exist "%BLD%" rmdir /s /q "%BLD%"
    echo [clean] build directory removed.
    exit /b 0
)

REM -------------------------- toolchain discovery ----------------------------
set "TCBIN="
for %%D in (
    "%RISCV_GCC_BIN%"
    "C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC12\bin"
    "C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC15\bin"
    "C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin"
) do (
    if not defined TCBIN if exist "%%~D\riscv-wch-elf-gcc.exe" set "TCBIN=%%~D"
    if not defined TCBIN if exist "%%~D\riscv32-wch-elf-gcc.exe" set "TCBIN=%%~D"
    if not defined TCBIN if exist "%%~D\riscv-none-embed-gcc.exe" set "TCBIN=%%~D"
)

if not defined TCBIN (
    for %%X in (riscv-wch-elf-gcc.exe riscv32-wch-elf-gcc.exe riscv-none-embed-gcc.exe) do (
        where %%X >nul 2>nul && set "TCBIN=%%~dpX"
    )
)

if not defined TCBIN (
    echo [ERROR] RISC-V GCC toolchain not found.
    echo         Install MounRiver Studio 2 and rerun, or set:
    echo         set "RISCV_GCC_BIN=C:\path\to\RISC-V Embedded GCC\bin"
    exit /b 1
)

if exist "%TCBIN%\riscv-wch-elf-gcc.exe" (
    set "PREF=riscv-wch-elf"
) else if exist "%TCBIN%\riscv32-wch-elf-gcc.exe" (
    set "PREF=riscv32-wch-elf"
) else (
    set "PREF=riscv-none-embed"
)
set "CC=%TCBIN%\%PREF%-gcc.exe"
set "OBJCOPY=%TCBIN%\%PREF%-objcopy.exe"
set "SIZE=%TCBIN%\%PREF%-size.exe"

if not exist "%BLD%" mkdir "%BLD%"

REM ------------------------------- compile ------------------------------------
set "CFLAGS=-march=rv32ecxw -mabi=ilp32e -Os -ffunction-sections -fdata-sections"
set "CFLAGS=%CFLAGS% -nostartfiles -flto -Wall"
set "INC=-I "%ROOT%SDK_inc" -I "%ROOT%User""

echo ------------------------------------------------------------
echo  Proteus32 bootloader build
echo  Toolchain : "%CC%"
echo  Flags     : %CFLAGS%
echo ------------------------------------------------------------

set "ERR=0"
set "OBJS="

REM Full rebuild every run: the cheap comparison of source timestamps is
REM unreliable in batch files, and a stale .o silently corrupts the build.
for %%S in (
    "Startup\startup_ch32v00x.S"
    "User\main.c"
    "User\iap.c"
    "User\flash.c"
    "User\ch32v00x_it.c"
    "User\system_ch32v00x.c"
    "SDK_src\ch32v00x_flash.c"
    "SDK_src\ch32v00x_rcc.c"
    "SDK_src\ch32v00x_usart.c"
) do (
    set "OBJ=%%~nxS.o"
    echo [CC] %%~S
    "%CC%" %CFLAGS% %INC% -c "%ROOT%%%~S" -o "%BLD%\!OBJ!"
    if errorlevel 1 set "ERR=1"
    set "OBJS=!OBJS! "%BLD%\!OBJ!""
)

if "%ERR%"=="1" (
    echo [ERROR] compilation failed.
    exit /b 1
)

REM --------------------------------- link -------------------------------------
REM -flto needs the flags at link time as well.
echo [LD] bootloader.elf
"%CC%" %CFLAGS% %OBJS% -T "%ROOT%Ld\Link.ld" --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=%BLD%\bootloader.map -o "%BLD%\bootloader.elf"
if errorlevel 1 (
    echo [ERROR] link failed.
    exit /b 1
)

REM ------------------------------ binary + size --------------------------------
"%OBJCOPY%" -O binary "%BLD%\bootloader.elf" "%BLD%\bootloader.bin"
if errorlevel 1 (
    echo [ERROR] objcopy failed.
    exit /b 1
)

echo.
echo ------------------------------ build sizes ------------------------------
"%SIZE%" "%BLD%\bootloader.elf"

for %%F in ("%BLD%\bootloader.bin") do set "BINSIZE=%%~zF"
echo.
echo bootloader.bin : %BINSIZE% B  [target max 1888 B, hard limit 1920 B]

if %BINSIZE% GTR 1920 (
    echo [ERROR] size %BINSIZE% B exceeds the hard boot-area limit of 1920 B.
    echo         Trim per TZ 3.1: READ command, granular statuses,
    echo         UART1_SendMultiyData, VERIFY tail.
    exit /b 1
)
if %BINSIZE% GTR 1888 (
    echo [WARN] size %BINSIZE% B is above the 1888 B target, still within
    echo        the 1920 B hard limit.
)
echo [OK] build succeeded: %BLD%\bootloader.bin
exit /b 0