<#
.SYNOPSIS
  Windows 빌드 편의 스크립트 (MSVC + vcpkg(FFmpeg) + Qt6 + CMake).

.DESCRIPTION
  vcpkg 매니페스트 모드(vcpkg.json)로 FFmpeg를 자동 설치하고, Qt6를 CMAKE_PREFIX_PATH로 잡아
  Ninja(기본) 또는 Visual Studio 생성기로 구성/빌드한다. POST_BUILD에서 windeployqt가 Qt DLL을 동봉한다.

.PARAMETER QtDir      Qt6 msvc 디렉터리. 예: C:\Qt\6.7.2\msvc2022_64  (필수)
.PARAMETER VcpkgRoot  vcpkg 클론 루트. 예: C:\dev\vcpkg  (필수, scripts\buildsystems\vcpkg.cmake 포함)
.PARAMETER Config     Release(기본) | Debug | RelWithDebInfo
.PARAMETER Generator  Ninja(기본) | "Visual Studio 17 2022"
.PARAMETER Clean      build 디렉터리를 지우고 새로 구성

.EXAMPLE
  .\build.ps1 -QtDir C:\Qt\6.7.2\msvc2022_64 -VcpkgRoot C:\dev\vcpkg
  .\build.ps1 -QtDir C:\Qt\6.7.2\msvc2022_64 -VcpkgRoot C:\dev\vcpkg -Generator "Visual Studio 17 2022" -Clean

.NOTES
  반드시 "x64 Native Tools Command Prompt for VS 2022"에서 실행하거나, VS 환경이 잡힌 PowerShell에서 실행.
  (Ninja 생성기는 cl.exe가 PATH에 있어야 함)
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$QtDir,
  [Parameter(Mandatory = $true)][string]$VcpkgRoot,
  [ValidateSet('Release','Debug','RelWithDebInfo')][string]$Config = 'Release',
  [string]$Generator = 'Ninja',
  [switch]$Clean
)
$ErrorActionPreference = 'Stop'
$Root  = (Resolve-Path "$PSScriptRoot\..\..").Path
$Build = Join-Path $Root 'build'

$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) { throw "vcpkg 툴체인 없음: $toolchain (VcpkgRoot 확인)" }
if (-not (Test-Path (Join-Path $QtDir 'lib\cmake\Qt6'))) { throw "Qt6 cmake 패키지 없음: $QtDir (QtDir 확인)" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake 가 PATH에 없습니다.' }

if ($Clean -and (Test-Path $Build)) { Remove-Item $Build -Recurse -Force }

# ── configure ──
$cfgArgs = @(
  '-S', $Root, '-B', $Build,
  '-G', $Generator,
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
  "-DCMAKE_PREFIX_PATH=$QtDir",
  '-DVCPKG_TARGET_TRIPLET=x64-windows'
)
if ($Generator -eq 'Ninja') {
  $cfgArgs += "-DCMAKE_BUILD_TYPE=$Config"
} else {
  $cfgArgs += @('-A', 'x64')   # VS 생성기는 다중구성 — 빌드 단계에서 --config 지정
}
Write-Host "==> cmake configure ($Generator / $Config)" -ForegroundColor Cyan
& cmake @cfgArgs
if ($LASTEXITCODE -ne 0) { throw "configure 실패 (rc=$LASTEXITCODE)" }

# ── build ──
Write-Host "==> cmake build" -ForegroundColor Cyan
& cmake --build $Build --config $Config -j
if ($LASTEXITCODE -ne 0) { throw "build 실패 (rc=$LASTEXITCODE)" }

# ── exe 위치 안내 ──
$exe = @(
  "$Build\src\new_viewer.exe",
  "$Build\src\$Config\new_viewer.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
Write-Host ""
Write-Host "빌드 완료: $exe" -ForegroundColor Green
Write-Host "테스트 실행:  ctest --test-dir `"$Build`" -C $Config --output-on-failure" -ForegroundColor Green
