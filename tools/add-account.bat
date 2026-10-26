@echo off
setlocal EnableExtensions
rem ===========================================================================
rem  ProjectPAULA - add a player account
rem
rem  Usage:  tools\add-account.bat <username> [gm_level]
rem    username  1-18 characters: letters A-Z a-z, digits 0-9 and _ (not case
rem              sensitive). The player starts PaulaLauncher.exe with this name.
rem              v0.1 has no password: the name is the key, so hand out names
rem              that are hard to guess.
rem    gm_level  optional, 0-255, default 0: accounts.gm_level. An account with
rem              gm_level above 0 also sees the islands that are not open to
rem              all. GM commands belong to a character, not to the account:
rem              see the hint printed at the end.
rem  Each setting can also come from the environment: PAULA_MYSQL,
rem  PAULA_DB_HOST, PAULA_DB_PORT, PAULA_DB_USER, PAULA_DB_PASS, PAULA_DB_NAME.
rem ===========================================================================

rem ---- settings (edit me: the same values as in install-db.bat) -------------
set "MYSQL=mysql.exe"
set "DB_HOST=127.0.0.1"
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

set "ACC_NAME=%~1"
set "ACC_GM=%~2"
if not defined ACC_NAME goto :usage
if not defined ACC_GM set "ACC_GM=0"

rem Checked through the environment, so any character typed is harmless here.
powershell -NoProfile -Command "if ($env:ACC_NAME -cmatch '^[A-Za-z0-9_]{1,18}\z') { exit 0 } else { exit 1 }" || (
    echo [ERROR] Invalid username: 1-18 characters, letters A-Z a-z, digits 0-9 and _ only.
    exit /b 1
)
powershell -NoProfile -Command "if ($env:ACC_GM -match '^[0-9]{1,3}\z' -and [int]$env:ACC_GM -le 255) { exit 0 } else { exit 1 }" || (
    echo [ERROR] Invalid gm_level: a number from 0 to 255.
    exit /b 1
)

rem A client given as a file that exists is called by its full path.
if not exist "%MYSQL%" goto :find_client
for %%M in ("%MYSQL%") do set "MYSQL=%%~fM"
goto :have_client
:find_client
where "%MYSQL%" >nul 2>&1 && goto :have_client
echo [ERROR] The MariaDB / MySQL client was not found: %MYSQL%
echo         Set MYSQL at the top of tools\add-account.bat to the full path of mysql.exe.
exit /b 1
:have_client

rem The password goes through the environment, not the command line.
set "MYSQL_PWD=%DB_PASS%"
"%MYSQL%" --host=%DB_HOST% --port=%DB_PORT% --user=%DB_USER% --protocol=TCP --default-character-set=utf8mb4 "%DB_NAME%" -e "INSERT INTO accounts (username, password_hash, gm_level) VALUES ('%ACC_NAME%', '', %ACC_GM%);" || (
    echo [ERROR] The account was not added - see the message above.
    echo         ERROR 1062 "Duplicate entry" means the name is already taken.
    echo         ERROR 1146 "doesn't exist" means the tables are missing: run install-db.bat.
    exit /b 1
)

echo Account "%ACC_NAME%" added, gm_level %ACC_GM%.
echo The player starts the game with:  PaulaLauncher.exe %ACC_NAME%
echo (or Account=%ACC_NAME% in launcher.ini).
echo.
echo To make one of its characters a GM, after the player created it in the game:
echo   UPDATE characters SET authority=1 WHERE name='CharacterName';
echo   (authority 0 = player, 1 = GM, 2 = deleted: never set 2 by hand)
endlocal
exit /b 0

:usage
echo Usage: tools\add-account.bat ^<username^> [gm_level]
echo   username  1-18 characters: letters A-Z a-z, digits 0-9 and _
echo   gm_level  optional, 0-255 (default 0)
pause
exit /b 1
