<#
.SYNOPSIS
  M1~M4 통합 스트레스 (Windows). 장비 READ-ONLY. ops/m1m4-stress-1h.sh의 PowerShell 포팅.

.DESCRIPTION
  채널: 장비 1 (relay 경유, M4) + 합성 N (직결, 별도 mediamtx 8654, M2 멀티채널). 전채널 녹화+스냅샷(M3).
  단계: 지속 -> viewer churn(M4 보호막) -> 장애주입(relay kill/자가복구 + 합성1개 drop/restore) -> 마무리.
  핵심측정: viewer 껐다켜도 장비 close ~0(M4 멱등 보호막), 채널독립성, 녹화/스냅샷 무결성, 누수0/크래시0.

  ※ Windows 특이점: relay와 합성 소스가 둘 다 mediamtx.exe라, 장애주입 때 커맨드라인(설정파일 경로)으로
    relay 인스턴스만 정확히 종료한다(합성 9채널 독립성 보존). relay 복구는 실행 중 viewer의
    RelayCoordinator 자가복구(ensureUp 재호출)에 의존 — 클린 sc stop/start 검증은 P5 relay-verify에서 별도.

.PARAMETER Sus  지속 단계 초 (기본 600)
.PARAMETER Chn  churn 단계 초 (기본 450)
.PARAMETER Fau  장애주입 단계 초 (기본 450, 최소 200)
.PARAMETER Fin  마무리 단계 초 (기본 300)
  → 기본 합계 1800s = 30분.

.EXAMPLE
  .\m1m4-stress.ps1                     # 30분 (600/450/450/300)
  .\m1m4-stress.ps1 -Sus 1200 -Chn 900 -Fau 900 -Fin 600   # 1시간
  .\m1m4-stress.ps1 -Nsyn 9 -DeviceUrl 'rtsp://169.254.4.1:8900/live'

.NOTES
  사전조건: 빌드 완료(build\src\...\new_viewer.exe), mediamtx.exe/ffmpeg.exe/ffprobe.exe/adb.exe PATH,
            VOXL2 장비 USB 연결(adb), 장비 RTSP 도달 가능.
#>
[CmdletBinding()]
param(
  [int]$Sus = 600,
  [int]$Chn = 450,
  [int]$Fau = 450,
  [int]$Fin = 300,
  [int]$Nsyn = 9,
  [string]$DeviceUrl = 'rtsp://169.254.4.1:8900/live'
)
$ErrorActionPreference = 'Continue'   # 스트레스: 개별 명령 실패가 전체를 중단시키지 않게

if ($Fau -lt 200) { throw 'Fau는 최소 200 이상이어야 합니다(장애주입 고정 시퀀스 200s).' }

# ── 경로 ──────────────────────────────────────────────────────────────────────
$Root   = (Resolve-Path "$PSScriptRoot\..\..").Path
$Fix    = Join-Path $Root 'tests\fixtures\h264.mkv'
$CfgDir = Join-Path $env:LOCALAPPDATA '영상관리시스템'        # QStandardPaths::AppConfigLocation
$Cfg    = Join-Path $CfgDir 'channels.json'
$RelayYml = Join-Path $CfgDir 'mediamtx.yml'                  # 앱이 생성하는 relay 설정
$SvcName  = 'NewViewerRelay'
$SnapSrc  = Join-Path ([Environment]::GetFolderPath('MyVideos')) 'new_viewer'   # MoviesLocation\new_viewer
$Ts  = Get-Date -Format 'yyyyMMdd-HHmm'
$Out = Join-Path $Root "logs\m1m4-$Ts"
$Rec = Join-Path $Out 'recs'
New-Item -ItemType Directory -Force -Path $Rec, (Join-Path $Out 'snaps') | Out-Null
$env:NV_RECORD_DIR = "$Rec\"
$Summary = Join-Path $Out 'summary.txt'
$DeviceLog = Join-Path $Out 'device.log'
$PerfLog   = Join-Path $Out 'perf.log'
New-Item -ItemType File -Force -Path (Join-Path $Out '.start') | Out-Null

# 앱 exe 자동 탐색 (Ninja 단일구성 / VS 다중구성)
$App = @(
  "$Root\build\src\new_viewer.exe",
  "$Root\build\src\Release\new_viewer.exe",
  "$Root\build\src\RelWithDebInfo\new_viewer.exe",
  "$Root\build\src\Debug\new_viewer.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $App) { throw "new_viewer.exe 를 찾을 수 없습니다. 먼저 빌드하세요. (build\src\...)" }

# ── 헬퍼 ──────────────────────────────────────────────────────────────────────
function Log($msg) {
  # Tee-Object -Append는 PS6+ 전용 — PS5.1 호환을 위해 콘솔/파일 분리
  $line = "[m1m4 $(Get-Date -Format 'HH:mm:ss')] $msg"
  Write-Host $line
  Add-Content -Path $Summary -Value $line
}
function Get-DevClose {
  if (Test-Path $DeviceLog) { @(Select-String -Path $DeviceLog -Pattern 'closing source pipe' -ErrorAction SilentlyContinue).Count } else { 0 }
}
function Get-HasSrc {
  try {
    $r = Invoke-RestMethod 'http://127.0.0.1:9997/v3/paths/list' -TimeoutSec 3
    if ($r.items -and $r.items[0].source) { 'yes' } else { 'no' }
  } catch { 'no' }
}
function Get-MtxCount { @(Get-Process mediamtx -ErrorAction SilentlyContinue).Count }
# relay mediamtx만 (설정파일 경로로 식별) — 합성(mediamtx-test.yml)은 보존
function Stop-RelayMtx {
  Get-CimInstance Win32_Process -Filter "Name='mediamtx.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and ($_.CommandLine -like '*영상관리시스템*') } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
$script:LA = 0
$script:LastProc = $null
function Launch($argString) {
  $script:LA++
  $outF = Join-Path $Out ("viewer-{0:D3}.out" -f $script:LA)
  $errF = Join-Path $Out ("viewer-{0:D3}.err" -f $script:LA)
  $argList = $argString -split '\s+' | Where-Object { $_ -ne '' }
  $script:LastProc = Start-Process $App -ArgumentList $argList `
    -RedirectStandardOutput $outF -RedirectStandardError $errF -PassThru -WindowStyle Minimized
}
function Kill-Viewer {
  if ($script:LastProc -and -not $script:LastProc.HasExited) {
    Stop-Process -Id $script:LastProc.Id -Force -ErrorAction SilentlyContinue
  }
  Get-Process new_viewer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
function Grep-Viewer($pattern) {
  @(Select-String -Path (Join-Path $Out 'viewer-*.out'), (Join-Path $Out 'viewer-*.err') `
      -Pattern $pattern -ErrorAction SilentlyContinue).Count
}

# ── 사전 클린 ──────────────────────────────────────────────────────────────────
Get-Process new_viewer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process mediamtx   -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process ffmpeg     -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
& sc.exe stop $SvcName   *>$null
& sc.exe delete $SvcName *>$null
& schtasks /delete /tn $SvcName /f *>$null
if (Test-Path $RelayYml) { Remove-Item $RelayYml -Force -ErrorAction SilentlyContinue }
Start-Sleep 2
New-Item -ItemType Directory -Force -Path $CfgDir | Out-Null
if (Test-Path $Cfg) { Copy-Item $Cfg (Join-Path $Out 'ch.bak') -Force }

# ── 합성 소스: 별도 mediamtx(8654/api9998) + N ffmpeg push ──────────────────────
$SynCfg = Join-Path $Out 'syn.yml'
@"
logLevel: warn
logDestinations: [stdout]
api: yes
apiAddress: 127.0.0.1:9998
rtsp: yes
rtspTransports: [tcp]
rtspAddress: 127.0.0.1:8654
rtpAddress: :8010
rtcpAddress: :8011
rtmp: no
hls: no
webrtc: no
srt: no
metrics: no
pprof: no
playback: no
paths:
  all_others:
"@ | Set-Content -Path $SynCfg -Encoding ascii   # BOM 없이 — mediamtx YAML 파서 BOM 거부 방지
Start-Process mediamtx -ArgumentList $SynCfg -RedirectStandardOutput (Join-Path $Out 'syn-mtx.log') -PassThru -WindowStyle Hidden | Out-Null
Start-Sleep 3
function Pub-Syn($cam) {
  # 발행자별 개별 로그 — Windows에서 여러 프로세스가 같은 파일을 열면 공유 위반
  Start-Process ffmpeg -ArgumentList @('-hide_banner','-loglevel','error','-re','-stream_loop','-1',
    '-i', $Fix, '-c','copy','-f','rtsp','-rtsp_transport','tcp',"rtsp://127.0.0.1:8654/$cam") `
    -RedirectStandardError (Join-Path $Out "syn-$cam.log") -PassThru -WindowStyle Hidden | Out-Null
}
for ($k = 1; $k -le $Nsyn; $k++) { Pub-Syn "cam$k"; Start-Sleep -Milliseconds 300 }
Start-Sleep 3
$cam1probe = (& ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 "rtsp://127.0.0.1:8654/cam1" 2>&1 | Select-Object -First 1)
Log "합성 소스 cam1: $cam1probe"

# ── channels.json: 장비1(relay) + 합성N(직결) ──────────────────────────────────
$chans = @( [ordered]@{ id='dev1'; name='장비'; url=$DeviceUrl; gridIndex=0; useRelay=$true } )
for ($k = 1; $k -le $Nsyn; $k++) {
  $chans += [ordered]@{ id="syn$k"; name="합성$k"; url="rtsp://127.0.0.1:8654/cam$k"; gridIndex=$k; useRelay=$false }
}
$json = ConvertTo-Json @($chans) -Depth 5 -Compress
[System.IO.File]::WriteAllText($Cfg, $json, (New-Object System.Text.UTF8Encoding($false)))  # BOM 없는 UTF-8
Log "channels.json: 장비1(relay) + 합성$Nsyn(직결) = $($chans.Count)채널"

# ── teardown ────────────────────────────────────────────────────────────────
function Invoke-Teardown {
  Log '=== teardown ==='
  Kill-Viewer
  & sc.exe stop $SvcName   *>$null
  & sc.exe delete $SvcName *>$null
  & schtasks /delete /tn $SvcName /f *>$null
  if (Test-Path $RelayYml) { Remove-Item $RelayYml -Force -ErrorAction SilentlyContinue }
  Get-Process ffmpeg   -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Get-Process mediamtx -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  if ($script:MonJob) { Stop-Job $script:MonJob -ErrorAction SilentlyContinue; Remove-Job $script:MonJob -Force -ErrorAction SilentlyContinue }
  if ($script:DevProc -and -not $script:DevProc.HasExited) { Stop-Process -Id $script:DevProc.Id -Force -ErrorAction SilentlyContinue }
  if (Test-Path (Join-Path $Out 'ch.bak')) { Copy-Item (Join-Path $Out 'ch.bak') $Cfg -Force }
}

try {
  # 장비 로그 (adb)
  $script:DevProc = Start-Process adb -ArgumentList @('shell','journalctl -u voxl-streamer -f --output short-iso') `
    -RedirectStandardOutput $DeviceLog -PassThru -WindowStyle Hidden -ErrorAction SilentlyContinue

  # 성능 모니터 (60s 간격: RSS/CPU/mtx/hasSrc)
  $script:MonJob = Start-Job -ArgumentList $PerfLog -ScriptBlock {
    param($perfLog)
    while ($true) {
      $p1 = Get-Process new_viewer -ErrorAction SilentlyContinue | Select-Object -First 1
      if ($p1) {
        $t1 = $p1.TotalProcessorTime.TotalMilliseconds; $w1 = [DateTime]::Now
        Start-Sleep -Milliseconds 1000
        $p2 = Get-Process -Id $p1.Id -ErrorAction SilentlyContinue
        if ($p2) {
          $rss = [math]::Round($p2.WorkingSet64 / 1MB)
          $dt = ([DateTime]::Now - $w1).TotalMilliseconds
          $cpu = if ($dt -gt 0) { [math]::Round((($p2.TotalProcessorTime.TotalMilliseconds - $t1) / $dt) * 100) } else { 0 }
          $mtx = @(Get-Process mediamtx -ErrorAction SilentlyContinue).Count
          $hs = try { $r = Invoke-RestMethod 'http://127.0.0.1:9997/v3/paths/list' -TimeoutSec 3; if ($r.items -and $r.items[0].source) {'yes'} else {'no'} } catch {'no'}
          "$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds()) rss_mb=$rss cpu=$cpu mtx=$mtx hasSrc=$hs" | Add-Content $perfLog
        }
      }
      Start-Sleep 59
    }
  }

  Log "start $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') 채널=$($Nsyn+1)(장비1 relay+합성$Nsyn 직결) 지속=$Sus churn=$Chn 장애=$Fau 마무리=$Fin"
  $devProbe = (& ffprobe -rtsp_transport tcp -v error -show_entries stream=codec_name -of csv=p=0 $DeviceUrl 2>&1 | Select-Object -First 1)
  Log "장비: $devProbe"

  # ── 1) 지속 ──────────────────────────────────────────────────────────────────
  Log "[지속] $($Nsyn+1)채널 기동(viewer relay ensureUp + 직결$Nsyn) ${Sus}s"
  Launch '--connect --auto-record --snapshot-every=30'; Start-Sleep 25
  Log "  mediamtx 인스턴스=$(Get-MtxCount)(relay+합성=2 기대) 장비 hasSource=$(Get-HasSrc) first_frame=$(Grep-Viewer 'first frame presented')"
  Start-Sleep ($Sus - 25); Kill-Viewer; Start-Sleep 3

  # ── 2) viewer churn (relay 유지, 장비 close 증가 0 기대) ───────────────────────
  Log "[churn] viewer 껐다키고 (relay 유지, 장비 close 증가 없어야) ${Chn}s"
  $cb = Get-DevClose; $cend = (Get-Date).AddSeconds($Chn); $k = 0
  $modes = @('--connect --snapshot-every=20','--connect --record-toggle=12','--connect --auto-record --snapshot-every=15')
  while ((Get-Date) -lt $cend) {
    $k++; Launch $modes[$k % 3]; Start-Sleep (70 + (Get-Random -Max 40))
    Kill-Viewer; Start-Sleep (8 + (Get-Random -Max 15))
  }
  $ca = Get-DevClose
  Log "  churn 완료: 재기동 ${k}회, 장비 close 증가 $($ca - $cb) (멱등 보호막=0 기대)"

  # ── 3) 장애주입 (relay kill/자가복구 + 합성 cam3 drop/restore) ─────────────────
  Log "[장애] ${Fau}s"
  Launch '--connect --auto-record --snapshot-every=30'; Start-Sleep 30
  Log "  relay mediamtx kill — 장비채널만 RelayDown, 합성$Nsyn 무영향(독립성) / viewer 자가복구 대기"
  Stop-RelayMtx; Start-Sleep 60
  Log "  relay 자가복구 확인: hasSource=$(Get-HasSrc) mediamtx=$(Get-MtxCount)"
  Start-Sleep 30
  Log "  합성 cam3 drop — syn3만 끊기고 타채널 무영향(M2 독립성)"
  Get-CimInstance Win32_Process -Filter "Name='ffmpeg.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -like '*8654/cam3*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  Start-Sleep 40
  Log "  합성 cam3 restore"; Pub-Syn 'cam3'; Start-Sleep 40
  Log "  장애 후 장비 hasSource=$(Get-HasSrc) mediamtx=$(Get-MtxCount)"
  Start-Sleep ($Fau - 200); Kill-Viewer; Start-Sleep 3

  # ── 4) 마무리 ──────────────────────────────────────────────────────────────────
  Log "[마무리] ${Fin}s"
  Launch '--connect --auto-record --snapshot-every=30'; Start-Sleep $Fin; Kill-Viewer; Start-Sleep 3

  Kill-Viewer; Start-Sleep 2

  # ── 판정 ───────────────────────────────────────────────────────────────────────
  Log '=== 판정 ==='
  Log "총 viewer 기동: $($script:LA) / 비정상로그: $(Grep-Viewer 'crash|abort|signal SIG|terminate|Sanitizer')"
  Log "first frame presented(누적): $(Grep-Viewer 'first frame presented')"
  Log "RelayDown=$(Grep-Viewer 'RelayDown') (M4 장비채널 진단) / Failed=$(Grep-Viewer '-> Failed')"
  Log "★ 장비 close/reopen 총: $(Get-DevClose) (churn 구간 증가 $($ca - $cb); M4 보호막)"

  # 스냅샷 수거 (MoviesLocation\new_viewer\*.png 중 .start 이후 생성분)
  $startT = (Get-Item (Join-Path $Out '.start')).LastWriteTime
  if (Test-Path $SnapSrc) {
    Get-ChildItem $SnapSrc -Filter *.png -ErrorAction SilentlyContinue |
      Where-Object { $_.LastWriteTime -gt $startT } |
      Move-Item -Destination (Join-Path $Out 'snaps') -Force -ErrorAction SilentlyContinue
  }

  # 녹화 무결성 (ffprobe로 video stream 디코드 가능 여부)
  $ok = 0; $bad = 0
  Get-ChildItem $Rec -Filter *.mkv -ErrorAction SilentlyContinue | ForEach-Object {
    if ($_.Length -gt 0) {
      & ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 $_.FullName *>$null
      if ($LASTEXITCODE -eq 0) { $ok++ } else { $bad++ }
    } else { $bad++ }
  }
  Log "녹화: 재생가능 $ok / 손상 $bad"
  $uniq = @(Get-ChildItem $Rec -Filter *.mkv -ErrorAction SilentlyContinue |
    ForEach-Object { ($_.BaseName -replace '_\d{8}_.*$','') } | Sort-Object -Unique).Count
  Log "고유 녹화채널: $uniq ($($Nsyn+1) 기대)"
  $sok = 0
  Get-ChildItem (Join-Path $Out 'snaps') -Filter *.png -ErrorAction SilentlyContinue | ForEach-Object {
    & ffprobe -v error $_.FullName *>$null; if ($LASTEXITCODE -eq 0) { $sok++ }
  }
  Log "스냅샷: 정상 $sok"

  # RSS/CPU 요약
  if (Test-Path $PerfLog) {
    $rss = @(Select-String -Path $PerfLog -Pattern 'rss_mb=(\d+)' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    $cpu = @(Select-String -Path $PerfLog -Pattern 'cpu=(\d+)'    | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
    if ($rss.Count) {
      $n = $rss.Count; $q = [math]::Max(1, [int]($n / 4))
      $first = [int](($rss | Select-Object -First $q | Measure-Object -Average).Average)
      $last  = [int](($rss | Select-Object -Last  $q | Measure-Object -Average).Average)
      Log ("  RSS(MB) 시작={0} 끝={1} 최대={2} 전반quarter={3} 후반quarter={4} 샘플={5}" -f $rss[0], $rss[-1], ($rss | Measure-Object -Maximum).Maximum, $first, $last, $n)
    }
    if ($cpu.Count) {
      Log ("  CPU(%) 평균={0} 최대={1}" -f [int](($cpu | Measure-Object -Average).Average), ($cpu | Measure-Object -Maximum).Maximum)
    }
  }
  Log "DONE $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') — $Summary"
}
finally {
  Invoke-Teardown
}
