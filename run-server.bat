@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  ProjectPAULA - start the four servers
rem
rem  Order: MasterServer, wait until it listens, then AuthServer, LoginServer and
rem  GameServer. LoginServer retries the MasterServer every 3 s, but starting
rem  the MasterServer first spares it a failed first attempt.
rem  A server that is already running is never started twice and never killed:
rem  stop the servers with stop-server.bat. Only the servers of THIS folder
rem  count: a process is one of them when its executable is this folder's
rem  MasterServer.exe, AuthServer.exe, LoginServer.exe or GameServer.exe,
rem  compared by full path. A program with the same name anywhere else (another
rem  ProjectPAULA folder, another game) is ignored.
rem  Each server writes <name>_live.log in this folder; the logs of the previous
rem  run are moved to logs\ first, with a timestamp.
rem  Needs: MariaDB running (db_* in conf\*.conf), the latest Microsoft Visual
rem  C++ v14 Redistributable (x86), and libmysql.dll next to the exes.
rem ===========================================================================
title ProjectPAULA - start
cd /d "%~dp0"
set "HERE=%~dp0"

rem  Must match master_port in conf\master_server.conf and game_port in
rem  conf\game_server.conf.
set "MASTER_PORT=9010"
set "GAME_PORT=8002"
rem  Seconds to wait for the MasterServer, and for the GameServer to load its data.
set "MASTER_WAIT=30"
set "GAME_WAIT=180"

rem --- the files the servers open (every path is relative to this folder) ---
for %%F in (MasterServer.exe AuthServer.exe LoginServer.exe GameServer.exe libmysql.dll) do (
    if not exist "%%F" ( echo [ERROR] %%F not found next to this script. & goto :fail )
)
for %%F in (master_server auth_server login_server game_server) do (
    if not exist "conf\%%F.conf" ( echo [ERROR] conf\%%F.conf not found. & goto :fail )
)
if not exist "tools\capture\sbox_cora.bin" ( echo [ERROR] tools\capture\sbox_cora.bin not found. & goto :fail )
if not exist "db\item_db.yml" ( echo [ERROR] The db\ data files were not found. & goto :fail )
if not exist "data\map\*.md3" ( echo [ERROR] The data\map\*.md3 files were not found. & goto :fail )

rem --- never start a second copy, never kill a running one -------------------
for %%P in (MasterServer AuthServer LoginServer GameServer) do (
    call :running %%P.exe && ( echo [ERROR] This folder's %%P.exe is already running. Stop the servers with stop-server.bat first. & goto :fail )
)

rem --- keep the previous logs: each server starts a new *_live.log -----------
if exist "*_live.log" (
    if not exist "logs\" mkdir "logs"
    for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do (
        for %%L in (*_live.log) do move /Y "%%L" "logs\%%T_%%L" >nul
    )
)

echo Starting MasterServer (port %MASTER_PORT%, for the other servers only)...
start "PAULA Master" MasterServer.exe
call :wait_port MasterServer.exe %MASTER_PORT% %MASTER_WAIT% || (
    echo [ERROR] MasterServer is not listening on port %MASTER_PORT%. Last lines of master_live.log:
    call :tail master_live.log
    goto :fail
)

echo Starting AuthServer...
start "PAULA Auth" AuthServer.exe
echo Starting LoginServer...
start "PAULA Login" LoginServer.exe
echo Starting GameServer (port %GAME_PORT%). Loading the data takes a while...
start "PAULA Game" GameServer.exe
call :wait_port GameServer.exe %GAME_PORT% %GAME_WAIT% || (
    echo [ERROR] GameServer is not listening on port %GAME_PORT%. Last lines of game_live.log:
    call :tail game_live.log
    goto :fail
)

rem --- a server whose start failed has already closed its window ------------
call :alive AuthServer.exe auth_live.log || goto :fail
call :alive LoginServer.exe login_live.log || goto :fail

echo.
echo The four servers are running. Logs: master_live.log auth_live.log login_live.log game_live.log
echo Stop them with stop-server.bat, which lets the GameServer save the players first.
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
:running
rem %1 = exe name. errorlevel 0 when this folder's %1 is running.
call :pids %~1
if defined PIDS exit /b 0
exit /b 1

:pids
rem %1 = exe name. PIDS = the ids of the processes whose executable is this
rem folder's %1, compared by full path; undefined when none is running.
set "PIDS="
set "PAULA_EXE=%HERE%%~1"
for /f "delims=" %%I in ('powershell -NoProfile -Command "@(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $env:PAULA_EXE } | ForEach-Object { $_.ProcessId }) -join ' '"') do set "PIDS=%%I"
exit /b 0

:alive
rem %1 = exe name, %2 = its log. errorlevel 0 when this folder's %1 is still running.
call :running %~1 && exit /b 0
echo [ERROR] %~1 stopped right after it started. Last lines of %~2:
call :tail "%~2"
exit /b 1

:wait_port
rem %1 = exe name, %2 = TCP port, %3 = seconds. errorlevel 0 as soon as this
rem folder's %1 listens on the port (another program on that port does not
rem count); 1 when it does not within the time, or has exited.
set "PAULA_EXE=%HERE%%~1"
powershell -NoProfile -Command "$end = (Get-Date).AddSeconds(%3); while ((Get-Date) -lt $end) { $mine = @(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $env:PAULA_EXE } | ForEach-Object { $_.ProcessId }); if (-not $mine) { exit 1 }; if (Get-NetTCPConnection -State Listen -LocalPort %2 -ErrorAction SilentlyContinue | Where-Object { $mine -contains $_.OwningProcess }) { exit 0 }; Start-Sleep -Milliseconds 500 }; exit 1"
exit /b %errorlevel%

:tail
if exist "%~1" powershell -NoProfile -Command "Get-Content -LiteralPath '%~1' -Tail 15"
exit /b 0

:fail
echo.
pause
endlocal
exit /b 1
