# WinDirStat - Windows Directory Statistics
# Copyright © WinDirStat Team
#
# SPDX-License-Identifier: GPL-3.0-or-later
# Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

param([string] $OutputPath = (Join-Path $env:TEMP "wds-fat-tests-$([guid]::NewGuid().ToString('N'))"))

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$source = Join-Path $root 'windirstat'
$output = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory($output) | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $visualStudio) { throw 'Visual Studio C++ build tools are required.' }

# Compile the production recovery sources with a minimal precompiled-header substitute and test volume adapters.
$pch = Get-Content -LiteralPath (Join-Path $source 'pch.h') -Raw
$standard = [regex]::Matches($pch, '(?m)^#include <[a-z_]+>\r?$').Value -join "`n"
$types = "#pragma once`n#define NOMINMAX`n#include <windows.h>`n#include <winioctl.h>`n$standard`n"
$types += "#include `"SmartPointer.h`"`n"
$code = ''
foreach ($name in 'RecoveryShared', 'RecoveryNtfs', 'RecoveryExFat', 'RecoveryFat') {
    foreach ($extension in 'h', 'cpp') {
        $text = Get-Content -LiteralPath (Join-Path $source "$name.$extension") -Raw
        $text = $text -replace '(?m)^#include[^\r\n]*\r?\n|^#pragma once\r?\n', ''
        if ($extension -eq 'h') { $types += $text + "`n" } else { $code += $text + "`n" }
    }
}
[IO.File]::WriteAllText((Join-Path $output 'RecoveryTypes.h'), $types, [Text.UTF8Encoding]::new($true))
[IO.File]::WriteAllText((Join-Path $output 'RecoveryCode.inc'), $code, [Text.UTF8Encoding]::new($true))
$nativeSource = Join-Path $PSScriptRoot 'RecoveryFatTests.cpp'
$build = @"
@ECHO OFF
CALL "$visualStudio\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >NUL
IF ERRORLEVEL 1 EXIT /B 1
cl /nologo /std:c++latest /utf-8 /EHsc /O1 /Gy /MT /DNDEBUG /DUNICODE /D_UNICODE /I"$source" /I"$output" /Fo"$output\RecoveryFatTests.obj" /Fe"$output\RecoveryFatTests.exe" "$nativeSource" /link /OPT:REF /OPT:ICF user32.lib
EXIT /B %ERRORLEVEL%
"@
$buildPath = Join-Path $output 'Build.cmd'
[IO.File]::WriteAllText($buildPath, $build, [Text.ASCIIEncoding]::new())
& $buildPath
if ($LASTEXITCODE -ne 0) { throw 'Recovery test compilation failed.' }
& python (Join-Path $PSScriptRoot 'RecoveryFatFixtures.py') $output
if ($LASTEXITCODE -ne 0) { throw "Recovery tests failed; fixtures are retained at $output" }
Write-Host "Recovery test report: $output\results.json"
