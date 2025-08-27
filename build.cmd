@ECHO OFF
TITLE Building WinDirStat
SETLOCAL

:: solicit whether this is production or beta build
ECHO Please choose a release type:
ECHO 1. Beta
ECHO 2. Production
SET /p CHOICE=Enter Choice (1 or 2): 
SET RELTYPE=
IF "%CHOICE%" EQU "1" SET RELTYPE=BETA
IF "%CHOICE%" EQU "2" SET RELTYPE=PRODUCTION
IF "%RELTYPE%" EQU "" EXIT /B 1

:: setup environment variables based on location of this script
SET THISDIR=%~dp0
SET THISDIR=%THISDIR:~0,-1%
SET BASEDIR=%THISDIR%\.
SET BLDDIR=%THISDIR%\build
SET PUBDIR=%THISDIR%\publish

:: cert info to use for signing
set TSAURL=http://time.certum.pl/
set LIBNAME=WinDirStat
set LIBURL=https://github.com/WinDirStat/WinDirStat

:: prepend preferred git and 7-zip paths
IF EXIST "%ProgramFiles%\7-Zip" SET PATH=%ProgramFiles%\7-Zip;%PATH%
IF EXIST "%ProgramFiles(x86)%\7-Zip" SET PATH=%ProgramFiles(x86)%\7-Zip;%PATH%
IF EXIST "%ProgramFiles%\Git" SET PATH=%ProgramFiles%\Git;%PATH%
IF EXIST "%ProgramFiles(x86)%\Git" SET PATH=%ProgramFiles(x86)%\Git;%PATH%

:: prepend preferred powershell path
FOR /F "DELIMS=" %%X IN ('DIR "%PROGRAMFILES%\PowerShell\pwsh.exe" /B /S /A') DO SET PATH=%PATH%;%%~dpX

:: prepend preferred system paths
SET PATH=%WINDIR%\system32;%WINDIR%\system32\WindowsPowerShell\v1.0;%PATH%

:: create define for build about box
SET PRODUCTION=0
IF /I "%RELTYPE%" EQU "PRODUCTION" SET PRODUCTION=1

:: import vs build tools
IF EXIST "%BLDDIR%" RD /S /Q "%BLDDIR%"
FOR /F "DELIMS=" %%X IN ('DIR "%ProgramFiles%\Microsoft Visual Studio\VsDevCmd.bat" /A /S /B') DO SET VS=%%X
CALL "%VS%"
IF EXIST "%WindowsSdkVerBinPath%\x86" msbuild "%BASEDIR%\windirstat.sln" /p:Configuration=Release /t:Clean;Build /p:Platform=Win32,ExternalCompilerOptions=/DPRODUCTION=%PRODUCTION%
IF EXIST "%WindowsSdkVerBinPath%\x64" msbuild "%BASEDIR%\windirstat.sln" /p:Configuration=Release /t:Clean;Build /p:Platform=x64,ExternalCompilerOptions=/DPRODUCTION=%PRODUCTION%
IF EXIST "%WindowsSdkVerBinPath%\arm64" msbuild "%BASEDIR%\windirstat.sln" /p:Configuration=Release /t:Clean;Build /p:Platform=ARM64,ExternalCompilerOptions=/DPRODUCTION=%PRODUCTION%
TIMEOUT /t 3 /nobreak >NUL

:: optimize executable size if pwsh is present
PWSH.EXE -Help >NUL 2>&1
IF %ERRORLEVEL% NEQ 0 ECHO PowerShell not found; skipping executable pruning
IF %ERRORLEVEL% EQU 0 FOR %%A IN (arm64 x86 x64) DO (
  PWSH -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Unrestricted -File "%THISDIR%\windirstat\Build\Prune Executable.ps1" "%BLDDIR%\WinDirStat_%%A.exe"
)

:: sign the main executables 
signtool sign /fd sha256 /tr %TSAURL% /td sha256 /d %LIBNAME% /du %LIBURL% "%BLDDIR%\*.exe"

:: build the msi
CALL "%THISDIR%\setup\build.cmd" "%RELTYPE%"

:: sign the msi
signtool sign /fd sha256 /tr %TSAURL% /td sha256 /d %LIBNAME% /du %LIBURL% "%BLDDIR%\*.msi"

:: copy the output files
IF EXIST "%PUBDIR%" RD /S /Q "%PUBDIR%"
FOR %%A IN (arm64 x86 x64) DO (
   IF NOT EXIST "%PUBDIR%\%%A" MKDIR "%PUBDIR%\%%A"
   COPY /Y "%BLDDIR%\WinDirStat_%%A.exe" "%PUBDIR%\%%A\WinDirStat.exe"
   COPY /Y "%BLDDIR%\WinDirStat_%%A.pdb" "%PUBDIR%\%%A\WinDirStat.pdb"
   COPY /Y "%BLDDIR%\WinDirStat-%%A.msi" "%PUBDIR%"
)

:: 7-zip executables and debug files
7z.EXE >NUL 2>&1
IF %ERRORLEVEL% NEQ 0 ECHO 7-Zip not found; skipping 7-Zip archive
IF %ERRORLEVEL% EQU 0 7z.EXE a -mx=9 "%PUBDIR%\WinDirStat.7z" "%PUBDIR%\*\*.exe"
IF %ERRORLEVEL% EQU 0 7z.EXE a -mx=9 "%PUBDIR%\WinDirStat-DebugSymbols.7z" "%PUBDIR%\*\*.pdb"
DEL /F /S /Q "%PUBDIR%\*.pdb" >NUL 2>&1

:: zip up executables
SET POWERSHELL=POWERSHELL.EXE -NoProfile -NonInteractive -NoLogo -ExecutionPolicy Unrestricted
PUSHD "%PUBDIR%"
FOR %%A IN (arm64 x86 x64) DO (
   %POWERSHELL% -Command "Compress-Archive '%PUBDIR%\%%A' -DestinationPath ('%PUBDIR%\WinDirStat.zip') -Update"
)
POPD


:: output hash information
SET HASHFILE=%PUBDIR%\WinDirStat-Hashes.txt
IF EXIST "%HASHFILE%" DEL /F "%HASHFILE%"
%POWERSHELL% -Command "Get-ChildItem -Include @('*.msi','*.exe','*.zip','*.7z') -Path '%PUBDIR%' -Recurse | Get-FileHash -Algorithm SHA256 | Out-File -Append '%HASHFILE%' -Width 256"
%POWERSHELL% -Command "Get-ChildItem -Include @('*.msi','*.exe','*.zip','*.7z') -Path '%PUBDIR%' -Recurse | Get-FileHash -Algorithm SHA1 | Out-File -Append '%HASHFILE%' -Width 256"
%POWERSHELL% -Command "Get-ChildItem -Include @('*.msi','*.exe','*.zip','*.7z') -Path '%PUBDIR%' -Recurse | Get-FileHash -Algorithm MD5 | Out-File -Append '%HASHFILE%' -Width 256"
%POWERSHELL% -Command "$Data = Get-Content '%HASHFILE%'; $Data.Replace((Get-Item -LiteralPath '%PUBDIR%').FullName + '\','').Trim() | Set-Content '%HASHFILE%'"

PAUSE