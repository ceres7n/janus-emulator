@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  ProjectPAULA - create or update the game database
rem
rem  Creates the database DB_NAME (utf8mb4) when it is missing, then runs every
rem  sql\NNN_*.sql in numeric order and stops at the first error. Running it
rem  again is safe: the scripts only add what is missing (CREATE TABLE IF NOT
rem  EXISTS, guarded column adds, INSERT IGNORE) and never drop anything.
rem  Use the same values as db_host, db_port, db_user, db_pass and db_name in
rem  conf\*.conf. Avoid the characters % ! ^ & " in the password.
rem  Each setting can also come from the environment: PAULA_MYSQL,
rem  PAULA_DB_HOST, PAULA_DB_PORT, PAULA_DB_USER, PAULA_DB_PASS, PAULA_DB_NAME.
rem ===========================================================================
title ProjectPAULA - install the database
cd /d "%~dp0"

rem ---- settings (edit me) ---------------------------------------------------
rem  The MariaDB / MySQL command-line client: mysql.exe when its folder is on
rem  PATH, otherwise its full path. WampServer, for example (edit me):
rem  C:\wamp64\bin\mariadb\mariadb11.4.9\bin\mysql.exe
set "MYSQL=mysql.exe"
set "DB_HOST=127.0.0.1"
rem  3307 is WampServer's MariaDB; a standalone MariaDB or MySQL uses 3306.
set "DB_PORT=3307"
set "DB_USER=root"
set "DB_PASS="
set "DB_NAME=janus"
rem ---------------------------------------------------------------------------
if defined PAULA_MYSQL set "MYSQL=%PAULA_MYSQL%"
if defined PAULA_DB_HOST set "DB_HOST=%PAULA_DB_HOST%"
if defined PAULA_DB_PORT set "DB_PORT=%PAULA_DB_PORT%"
if defined PAULA_DB_USER set "DB_USER=%PAULA_DB_USER%"
if defined PAULA_DB_PASS set "DB_PASS=%PAULA_DB_PASS%"
if defined PAULA_DB_NAME set "DB_NAME=%PAULA_DB_NAME%"

rem A client given as a file that exists is called by its full path.
if not exist "%MYSQL%" goto :find_client
for %%M in ("%MYSQL%") do set "MYSQL=%%~fM"
goto :have_client
:find_client
where "%MYSQL%" >nul 2>&1 && goto :have_client
echo [ERROR] The MariaDB / MySQL client was not found: %MYSQL%
echo         Set MYSQL at the top of install-db.bat to the full path of mysql.exe.
goto :fail
:have_client

if not defined DB_NAME goto :bad_name
for /f "delims=ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_" %%X in ("%DB_NAME%") do goto :bad_name
if not exist "sql\001_login_db.sql" (
    echo [ERROR] sql\001_login_db.sql not found next to this script.
    goto :fail
)

rem The password goes through the environment, not the command line.
set "MYSQL_PWD=%DB_PASS%"
set "MYSQL_ARGS=--host=%DB_HOST% --port=%DB_PORT% --user=%DB_USER% --protocol=TCP --default-character-set=utf8mb4"

echo Connecting to %DB_HOST%:%DB_PORT% as %DB_USER% ...
"%MYSQL%" %MYSQL_ARGS% -N -B -e "SELECT CONCAT('Server version: ', VERSION());" || (
    echo [ERROR] Cannot connect. Is MariaDB running, and are the settings at the top of this file right?
    goto :fail
)

echo Creating the database %DB_NAME% if it does not exist ...
"%MYSQL%" %MYSQL_ARGS% -e "CREATE DATABASE IF NOT EXISTS `%DB_NAME%` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;" || goto :fail

set "APPLIED=0"
for /f "delims=" %%F in ('dir /b /a-d /on "sql\*.sql" ^| findstr /R /I "^[0-9][0-9][0-9]_.*\.sql$"') do (
    echo Applying %%F ...
    "%MYSQL%" %MYSQL_ARGS% "%DB_NAME%" < "sql\%%F" || (
        echo [ERROR] sql\%%F failed - see the message above. The files after it were not applied.
        goto :fail
    )
    set /a APPLIED+=1
)

echo.
echo The database %DB_NAME% is ready: %APPLIED% scripts applied.
echo Next: add the player accounts with  tools\add-account.bat ^<name^> [gm_level]
echo.
pause
endlocal
exit /b 0

:bad_name
echo [ERROR] DB_NAME must be letters, digits and _ only.
goto :fail

:fail
echo.
pause
endlocal
exit /b 1
