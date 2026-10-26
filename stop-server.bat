@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  ProjectPAULA - stop the servers
rem
rem  Only the servers of THIS folder are stopped: a process is one of them when
rem  its executable is this folder's MasterServer.exe, AuthServer.exe,
rem  LoginServer.exe or GameServer.exe, compared by full path. A program with
rem  the same name anywhere else (another ProjectPAULA folder, another game) is
rem  never touched. Reach this folder by the same path run-server.bat used: a
rem  subst drive or a short 8.3 name of it counts as another path.
rem  Each server is first asked to close (taskkill /PID, without /F). Its console
rem  gets the close event, which the servers handle like Ctrl+C: the GameServer
rem  saves the online players before it exits. Order: GameServer, LoginServer,
rem  AuthServer, MasterServer.
rem  A server still running after STOP_WAIT seconds is forced (taskkill /F /PID)
rem  with a warning. A forced GameServer loses what changed since its last
rem  autosave.
rem ===========================================================================
title ProjectPAULA - stop
cd /d "%~dp0"
set "HERE=%~dp0"
set "STOP_WAIT=15"
set "FORCED=0"
set "FOUND=0"

for %%P in (GameServer LoginServer AuthServer MasterServer) do call :stop %%P.exe

call :hidden && (
    echo.
    echo [NOTE] A process with a server's name is running whose folder this script
    echo        cannot read - another user's, or one started as administrator. It
    echo        was not touched. If it is a server of this folder, run
    echo        stop-server.bat as administrator.
)
if "%FORCED%"=="1" (
    echo.
    echo [WARNING] A server did not close in time and was forced to stop - see above.
    pause
    endlocal
    exit /b 1
)
if "%FOUND%"=="0" (
    echo No server of this folder was running.
) else (
    echo All servers of this folder are stopped.
)
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
:stop
rem %1 = exe name. Asks this folder's %1 to close, waits, and forces it only as
rem a last resort - always by process id, never by name.
call :pids %~1
if not defined PIDS exit /b 0
set "FOUND=1"
echo Stopping %~1 (PID %PIDS%) ...
for %%I in (%PIDS%) do taskkill /PID %%I >nul 2>&1
call :wait_exit %~1 %STOP_WAIT% && ( echo   %~1 has stopped. & exit /b 0 )
echo [WARNING] %~1 is still running after %STOP_WAIT% seconds - forcing it to stop.
rem (the ids are read again: only a process that still is this folder's %1 is forced)
call :pids %~1
if defined PIDS for %%I in (%PIDS%) do taskkill /F /PID %%I >nul 2>&1
set "FORCED=1"
exit /b 0

:pids
rem %1 = exe name. PIDS = the ids of the processes whose executable is this
rem folder's %1, compared by full path; undefined when none is running.
set "PIDS="
set "PAULA_EXE=%HERE%%~1"
for /f "delims=" %%I in ('powershell -NoProfile -Command "@(Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $env:PAULA_EXE } | ForEach-Object { $_.ProcessId }) -join ' '"') do set "PIDS=%%I"
exit /b 0

:wait_exit
rem %1 = exe name, %2 = seconds. errorlevel 0 once no process runs this
rem folder's %1.
set "PAULA_EXE=%HERE%%~1"
powershell -NoProfile -Command "$end = (Get-Date).AddSeconds(%2); while (Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $env:PAULA_EXE }) { if ((Get-Date) -ge $end) { exit 1 }; Start-Sleep -Milliseconds 500 }; exit 0"
exit /b %errorlevel%

:hidden
rem errorlevel 0 when a process with one of the server names is running whose
rem executable path this script cannot read. It is only reported, never stopped.
powershell -NoProfile -Command "$names = 'MasterServer.exe', 'AuthServer.exe', 'LoginServer.exe', 'GameServer.exe'; if (Get-CimInstance Win32_Process | Where-Object { -not $_.ExecutablePath -and $names -contains $_.Name }) { exit 0 }; exit 1"
exit /b %errorlevel%
