<#
.SYNOPSIS
  로컬 합성 RTSP 소스 (Windows) — mediamtx + ffmpeg 루프 발행으로 cam1..camN 제공.
  ops/synthetic-source.sh(macOS)의 PowerShell 포팅. 스케일/펜아웃/녹화동시성 테스트용.

.DESCRIPTION
  장비 없이 재현 가능한 멀티채널 부하를 만든다. relay와 포트가 겹치지 않도록
  RTSP=8654 / API=9998 / RTP=8010 / RTCP=8011 을 쓴다(앱 relay 기본 8554/9997과 분리).

.EXAMPLE
  .\synthetic-source.ps1 start 9          # mediamtx 기동 + cam1..cam9 발행
  .\synthetic-source.ps1 stop             # 전체 종료
  .\synthetic-source.ps1 drop cam3        # cam3 발행 중단(독립성/재연결 유발)
  .\synthetic-source.ps1 restore cam3     # cam3 재발행
  .\synthetic-source.ps1 status

.NOTES
  사전조건: mediamtx.exe, ffmpeg.exe, ffprobe.exe 가 PATH에 있어야 함.
  (winget install Gyan.FFmpeg / mediamtx는 github 릴리스 exe를 PATH에 배치)
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)][string]$Command = 'help',
  [Parameter(Position = 1)][string]$Arg1,
  [Parameter(Position = 2)][string]$Arg2
)
$ErrorActionPreference = 'Stop'

# 스크립트-상대 ROOT (어느 워크트리/클론에서도 동작)
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Run  = Join-Path $Root 'logs\synth'
$Cfg  = Join-Path $Run  'mediamtx-test.yml'
$Fix  = Join-Path $Root 'tests\fixtures\h264.mkv'
$Port = 8654
$Api  = '127.0.0.1:9998'
New-Item -ItemType Directory -Force -Path $Run | Out-Null

function Require-Tool($name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) {
    throw "$name 이(가) PATH에 없습니다. (ffmpeg: winget install Gyan.FFmpeg / mediamtx: github 릴리스 exe를 PATH에)"
  }
}

function Write-Cfg {
@"
logLevel: warn
logDestinations: [stdout]
api: true
apiAddress: $Api
rtsp: true
rtspTransports: [tcp]
rtspAddress: 127.0.0.1:$Port
rtpAddress: :8010
rtcpAddress: :8011
rtmp: false
hls: false
webrtc: false
srt: false
paths:
  all_others:
"@ | Set-Content -Path $Cfg -Encoding ascii   # BOM 없이 — mediamtx YAML 파서 BOM 거부 방지
}

function Pub-One($cam, $fixture = $Fix) {
  $log = Join-Path $Run "$cam.log"
  $p = Start-Process ffmpeg `
    -ArgumentList @('-hide_banner','-loglevel','error','-re','-stream_loop','-1',
      '-i', $fixture, '-c','copy','-f','rtsp','-rtsp_transport','tcp',
      "rtsp://127.0.0.1:$Port/$cam") `
    -RedirectStandardError $log -PassThru -WindowStyle Hidden
  $p.Id | Set-Content (Join-Path $Run "$cam.pid")
}

function Stop-ByPidFile($pidFile) {
  if (Test-Path $pidFile) {
    $procId = Get-Content $pidFile | Select-Object -First 1
    Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
    Remove-Item $pidFile -ErrorAction SilentlyContinue
  }
}

switch ($Command) {
  'start' {
    $n = [int]$Arg1
    if ($n -lt 1) { throw '사용: start N (N>=1)' }
    if (-not (Test-Path $Fix)) { throw "fixture 없음: $Fix" }
    Require-Tool mediamtx; Require-Tool ffmpeg; Require-Tool ffprobe
    Write-Cfg
    Stop-ByPidFile (Join-Path $Run 'mediamtx.pid')
    Start-Sleep 1
    $mtx = Start-Process mediamtx -ArgumentList $Cfg `
      -RedirectStandardOutput (Join-Path $Run 'mediamtx.log') -PassThru -WindowStyle Hidden
    $mtx.Id | Set-Content (Join-Path $Run 'mediamtx.pid')
    Start-Sleep 2
    for ($k = 1; $k -le $n; $k++) { Pub-One "cam$k"; Start-Sleep -Milliseconds 300 }
    Start-Sleep 2
    Write-Host "기동 완료: mediamtx + cam1..cam$n"
    $probe = & ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name `
      -of csv=p=0 "rtsp://127.0.0.1:$Port/cam1" 2>&1
    if ($probe -match 'h264|hevc|h265') { Write-Host "cam1 OK ($probe)" }
    else { Write-Host "cam1 probe 실패: $probe" }
  }
  'stop' {
    Get-ChildItem (Join-Path $Run 'cam*.pid') -ErrorAction SilentlyContinue | ForEach-Object { Stop-ByPidFile $_.FullName }
    Stop-ByPidFile (Join-Path $Run 'mediamtx.pid')
    Write-Host '합성 소스 종료'
  }
  'drop' {
    if (-not $Arg1) { throw '사용: drop camK' }
    $pf = Join-Path $Run "$Arg1.pid"
    if (Test-Path $pf) { Stop-ByPidFile $pf; Write-Host "$Arg1 발행 중단" } else { Write-Host "$Arg1 없음" }
  }
  'restore' {
    if (-not $Arg1) { throw '사용: restore camK [fixture]' }
    $fx = if ($Arg2) { $Arg2 } else { $Fix }
    Pub-One $Arg1 $fx; Write-Host "$Arg1 재발행"
  }
  'status' {
    $mtxPid = Join-Path $Run 'mediamtx.pid'
    $up = (Test-Path $mtxPid) -and (Get-Process -Id (Get-Content $mtxPid) -ErrorAction SilentlyContinue)
    Write-Host "mediamtx: $(if ($up) {'up'} else {'down'})"
    Get-ChildItem (Join-Path $Run 'cam*.pid') -ErrorAction SilentlyContinue | ForEach-Object {
      $c = $_.BaseName
      $alive = Get-Process -Id (Get-Content $_.FullName) -ErrorAction SilentlyContinue
      Write-Host "$c`: $(if ($alive) {'up'} else {'down'})"
    }
    try { (Invoke-RestMethod "http://$Api/v3/paths/list" -TimeoutSec 3 | ConvertTo-Json -Compress) } catch { Write-Host 'API 응답 없음' }
  }
  default {
    Write-Host '사용: synthetic-source.ps1 {start N|stop|drop camK|restore camK [fixture]|status}'
  }
}
