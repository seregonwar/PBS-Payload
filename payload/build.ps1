# Build the PS5 streaming payload with host clang and the PS5 Payload SDK.
# Set PS5_PAYLOAD_SDK or pass -SdkPath. The build embeds browser assets.

[CmdletBinding()]
param(
  [ValidateSet('PS5', 'PS4')][string]$Platform = 'PS5',
  [ValidateSet('stream', 'dump', 'probe', 'smoke', 'worker-probe')][string]$Kind = 'stream',
  [string]$WslDistribution = 'Debian',
  [string]$SdkPath,
  [string]$Output,
  [ValidateSet('Debug', 'Release')][string]$Config = 'Release',
  [ValidateSet(0, 1)][int]$Ps4Usage = 0,
  [ValidateSet('720p', '1080p')][string]$Ps4Resolution = '720p',
  [ValidateSet(30, 60)][int]$Ps4FrameRate = 60,
  [switch]$Ps4AdmissionProbe,
  [switch]$Ps4LatchBypass,
  [switch]$Ps4BudgetUnlock,
  [switch]$Ps4DaemonDump,
  [switch]$Ps4EncoderState,
  [switch]$Ps4LowMemory,
  [switch]$Ps4Remote1080,
  [switch]$Ps4VencJournal,
  [switch]$Ps4NativeSource,
  [ValidateSet(3,4,5)][int]$Ps4MemoryCount = 3,
  [switch]$Ps4ImagePaste,
  [switch]$Ps4NativeReadWait,
  [switch]$Ps4Observers,
  [ValidateSet('auto', 'remote', 'local')][string]$Ps4Avcap = 'auto',
  [switch]$Ps4RemoteLifecycle,
  [switch]$Ps4RemoteIdleRun,
  [ValidateSet(-1, 0, 1)][int]$Ps4WorkerBoot = -1,
  [ValidateRange(1, 3600)][int]$Ps4CaptureSeconds = 10,
  [switch]$SdkStyle,
  [switch]$TesterRelease,
  # Branding is opt-in so a default build carries no owner logo.
  [switch]$WebLogo,
  [switch]$Ps5NoNativeRateDefault,
  [ValidateSet(0, 1)][int]$Ps5Controller = 1,
  [switch]$Ps5OffsetDumpDefault,
  [switch]$Ps5DependencyDumpDefault,
  [switch]$Ps5ModuleFileDumpDefault,
  [switch]$Ps5DiagnosticOpenOnly,
  [ValidateSet('2160p', '1440p', '1080p', '720p')][string]$Ps5DefaultResolution = '1440p'
)

$ErrorActionPreference = 'Stop'

if ($Platform -eq 'PS4') {
  if ($Output) { throw 'PS4 outputs are written to payload/build/ps4. Omit -Output.' }
  if (-not $SdkPath) { $SdkPath = $env:PS4_PAYLOAD_SDK }
  if (-not $SdkPath) { throw 'Pass -SdkPath with the Linux path to ps4-payload-sdk, or set PS4_PAYLOAD_SDK.' }
  $windowsScript = (Join-Path $PSScriptRoot 'build.sh').Replace('\', '/')
  $scriptPath = & wsl -d $WslDistribution -- wslpath -a $windowsScript
  if ($LASTEXITCODE -ne 0) { throw 'Cannot translate build path for WSL.' }
  $scriptPath = $scriptPath.Trim()
  $imagePaste = if ($Ps4ImagePaste) { 1 } else { 0 }
  $nativeReadWait = if ($Ps4NativeReadWait) { 1 } else { 0 }
  $observers = if ($Ps4Observers) { 1 } else { 0 }
  $lifecycle = if ($Ps4RemoteLifecycle) { 1 } else { 0 }
  $idleRun = if ($Ps4RemoteIdleRun) { 1 } else { 0 }
  $admissionProbe = if ($Ps4AdmissionProbe) { 1 } else { 0 }
  $latchBypass = if ($Ps4LatchBypass) { 1 } else { 0 }
  $budgetUnlock = if ($Ps4BudgetUnlock) { 1 } else { 0 }
  $daemonDump = if ($Ps4DaemonDump) { 1 } else { 0 }
  $encoderState = if ($Ps4EncoderState) { 1 } else { 0 }
  $lowMemory = if ($Ps4LowMemory -or $Ps4Remote1080) { 1 } else { 0 }
  $nativeSource = if ($Ps4NativeSource) { 1 } else { 0 }
  $vencJournal = if ($Ps4VencJournal) { 1 } else { 0 }
  $remote1080 = if ($Ps4Remote1080) { 1 } else { 0 }
  $buildArgs = @('wsl', '-d', $WslDistribution, '--', 'env',
    "PS4_PAYLOAD_SDK=$SdkPath", "PBS_BUILD_CONFIG=$Config",
    ("PBS_PS4_CAPTURE_HEIGHT=" + $Ps4Resolution.TrimEnd('p')),
    "PBS_PS4_CAPTURE_FPS=$Ps4FrameRate",
    "PBS_PS4_ADMISSION_PROBE=$admissionProbe",
    "PBS_PS4_LATCH_BYPASS=$latchBypass",
    "PBS_PS4_BUDGET_UNLOCK=$budgetUnlock",
    "PBS_PS4_DAEMON_DUMP=$daemonDump",
    "PBS_PS4_ENCODER_STATE=$encoderState",
    "PBS_PS4_LOW_MEMORY=$lowMemory",
    "PBS_PS4_REMOTE_1080=$remote1080",
    "PBS_PS4_VENC_JOURNAL=$vencJournal",
    "PBS_PS4_NATIVE_SOURCE=$nativeSource",
    "PBS_PS4_MEMORY_COUNT=$Ps4MemoryCount",
    "PBS_PS4_CAPTURE_USAGE=$Ps4Usage", "PBS_PS4_IMAGE_PASTE=$imagePaste",
    "PBS_PS4_READ_WAIT_EVENTS=$nativeReadWait", "PBS_PS4_SMOKE_OBSERVERS=$observers",
    "PBS_PS4_REMOTE_LIFECYCLE=$lifecycle", "PBS_PS4_REMOTE_IDLE_RUN=$idleRun",
    "PBS_PS4_WORKER_BOOT=$Ps4WorkerBoot",
    "PBS_PS4_CAPTURE_SECONDS=$Ps4CaptureSeconds",
    "PBS_WEB_LOGO=$(if ($WebLogo) { 1 } else { 0 })",
    ("PBS_PS4_WORKER_PROBE=" + $(if ($env:PBS_PS4_WORKER_PROBE) { $env:PBS_PS4_WORKER_PROBE } else { "0" })))
  if ($Ps4Avcap -eq 'remote') { $buildArgs += 'PBS_PS4_REMOTE_AVCAP=1' }
  elseif ($Ps4Avcap -eq 'local') { $buildArgs += 'PBS_PS4_REMOTE_AVCAP=0' }
  $buildArgs += @('bash', $scriptPath, 'ps4', $Kind)
  $wslArgs = $buildArgs[1..($buildArgs.Count - 1)]
  & $buildArgs[0] @wslArgs
  if ($LASTEXITCODE -ne 0) { throw "PS4 build failed (exit $LASTEXITCODE)" }
  return
}
if ($Kind -ne 'stream' -and $Kind -ne 'dump') { throw 'This build kind is available for PS4 only.' }
if (-not $SdkPath) { $SdkPath = $env:PS5_PAYLOAD_SDK }

$generated = Join-Path $PSScriptRoot 'build/generated'
if ($Kind -eq 'stream') {
  New-Item -ItemType Directory -Force -Path $generated | Out-Null
  $viewer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264.html') -Raw -Encoding UTF8
  $muxer = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\h264-mux.js') -Raw -Encoding UTF8
  $viewer = $viewer.Replace('/*PBS_MUX*/', $muxer)
  $controller = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\controller.js') -Raw -Encoding UTF8
  $viewer = $viewer.Replace('/*PBS_CONTROLLER*/', $controller.Replace('</script', '<\/script'))
  $player = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'web\vendor\videojs-player.js') -Raw -Encoding UTF8
  $viewer = $viewer.Replace('/*PBS_VIDEOJS*/', $player.Replace('</script', '<\/script'))
  $escapedViewer = $viewer.Replace('\', '\\').Replace('"', '\"').Replace("`r", '').Replace("`n", '\n').Replace('?', '\?')
  $viewerHeader = '#pragma once' + "`n" + 'static const char PBS_H264_HTML[] = "' + $escapedViewer + '";' + "`n"
  [System.IO.File]::WriteAllText((Join-Path $generated 'webui.h'), $viewerHeader, [System.Text.UTF8Encoding]::new($false))
  if ($WebLogo) {
    $bannerBytes = [System.IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'web\banner.png'))
    $bannerHeader = '#pragma once' + "`n" + 'static const unsigned char PBS_BANNER_BYTES[] = {' + ($bannerBytes -join ',') + '};' + "`n" + 'static const size_t PBS_BANNER_LEN = sizeof PBS_BANNER_BYTES;' + "`n"
  } else {
    $bannerHeader = '#pragma once' + "`n" + 'static const unsigned char PBS_BANNER_BYTES[] = {0};' + "`n" + 'static const size_t PBS_BANNER_LEN = 0;' + "`n"
  }
  [System.IO.File]::WriteAllText((Join-Path $generated 'banner.h'), $bannerHeader, [System.Text.UTF8Encoding]::new($false))
}

$candidates = @(
  $SdkPath,
  (Join-Path $PSScriptRoot '..\..\sdk')
) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'target\include\ps5\kernel.h')) }

$sdk = $candidates | Select-Object -First 1
if (-not $sdk) {
  throw "PS5 Payload SDK not found. Set `$env:PS5_PAYLOAD_SDK or pass -SdkPath."
}
$sdk = (Resolve-Path $sdk).ProviderPath

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) {
  $local = Join-Path $sdk 'llvm\clang.exe'
  if (Test-Path $local) { $clang = $local }
}
if (-not $clang) { throw 'clang not found in PATH or the SDK.' }

if (-not $Output) {
  $Output = Join-Path $PSScriptRoot $(if ($Kind -eq 'dump') { 'build\pbs-ps5-dump.elf' } else { 'build\pbs.elf' })
}
$outDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$toolDir = if ($IsWindows -or $env:OS -eq 'Windows_NT') { Join-Path $sdk 'win' } else { Join-Path $sdk 'bin' }
if (Test-Path $toolDir) { $env:PATH = "$toolDir;$env:PATH" }

# The dump payloads are measurement tools: their entry points live under
# tools/dump/<platform>/, outside the capture payload sources.
$repoRoot = Split-Path -Parent $PSScriptRoot
$sourceNames = if ($Kind -eq 'dump') {
  @('platform/ps5/module_dump.c', 'tools/dump/ps5/dump_main.c')
} else {
  @('platform/ps5/main.c', 'platform/ps5/offset_dump.c', 'platform/ps5/module_dump.c',
    'platform/ps5/request_template.c', 'platform/ps5/worker.c', 'platform/ps4/worker/client.c',
    'platform/ps4/worker/remote.c', 'common/process/inject.c',
    'common/http/server.c', 'common/input/controller.c', 'common/config/config.c',
    'common/log/log.c')
}
if ($Kind -eq 'stream' -and $Ps5Controller -eq 1) {
  $sourceNames += 'platform/ps5/controller.c'
}
$sources = $sourceNames | ForEach-Object {
  if ($_ -like 'tools/*') { Join-Path $repoRoot $_ } else { Join-Path $PSScriptRoot ('src/' + $_) }
}
if ($Kind -eq 'stream') {
  $offsetDir = Join-Path $PSScriptRoot 'src/platform/ps5/offset'
  $sources += @(Get-ChildItem -LiteralPath $offsetDir -Filter '*.c' -File |
    Sort-Object Name | ForEach-Object { $_.FullName })
}

$kernelHeader = Join-Path $sdk 'target\include\ps5\kernel.h'
$sdkDefines = @()
$sdkDefines += "-DPBS_PS5_CONTROLLER=$Ps5Controller"
if($TesterRelease){$sdkDefines += '-DPBS_TESTER_RELEASE'}
if($Ps5NoNativeRateDefault){$sdkDefines += '-DPBS_PS5_NO_NATIVE_RATE_DEFAULT'}
if($Ps5OffsetDumpDefault){$sdkDefines += '-DPBS_PS5_OFFSET_DUMP_DEFAULT'}
if($Ps5DependencyDumpDefault){$sdkDefines += '-DPBS_PS5_DEPENDENCY_DUMP_DEFAULT'}
if($Ps5ModuleFileDumpDefault){$sdkDefines += '-DPBS_PS5_MODULE_FILE_DUMP_DEFAULT'}
if($Ps5DiagnosticOpenOnly){$sdkDefines += '-DPBS_PS5_DIAGNOSTIC_OPEN_ONLY'}
if($Ps5DefaultResolution -eq '1080p'){$sdkDefines += '-DPBS_PS5_DEFAULT_1080P'}
if($Ps5DefaultResolution -eq '720p'){$sdkDefines += '-DPBS_PS5_DEFAULT_720P'}
if($Ps5DefaultResolution -eq '2160p'){$sdkDefines += '-DPBS_PS5_DEFAULT_2160P'}
if ((Test-Path $kernelHeader) -and
    ((Get-Content -LiteralPath $kernelHeader -Raw) -match 'kernel_get_ucred_attrs\s*\(\s*pid_t\s+pid\s*,\s*uint8_t')) {
  $sdkDefines += '-DPBS_UCRED_ATTRS_ARRAY'
}

$includeFlags = @('-I', (Join-Path $PSScriptRoot 'src'), '-I', $generated)
$cflags = if ($Config -eq 'Debug') { @('-O0', '-g3') } else { @('-O2', '-g') }
$platformLibraries = if ($Kind -eq 'dump') {
  @('-lSceLibcInternal')
} else {
  @('-lSceLibcInternal', '-lSceNet', '-lSceRegMgr', '-lSceUserService',
    '-lSceRemoteplay', '-lSceSystemService', '-lSceAudioOut')
}

if ($SdkStyle) {
  $argsList = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    '-isysroot', $sdk,
    '-isystem', (Join-Path $sdk 'target\include'),
    '-L', (Join-Path $sdk 'target\lib'),
    '-L', (Join-Path $sdk 'target\user\homebrew\lib'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '-lc', '-lkernel_web',
    '--end-no-unused-arguments'
  ) + $includeFlags + $cflags + $sdkDefines + @(
    '-Wall', '-Wextra', '-Wno-unused-parameter'
  ) + $sources + @(
    '--start-no-unused-arguments',
    (Join-Path $sdk 'target\lib\crt1.o'),
    $platformLibraries,
    '--end-no-unused-arguments',
    '-o', $Output
  )
} else {
  $argsList = @(
    '--start-no-unused-arguments',
    '-target', 'x86_64-sie-ps5',
    "--sysroot=$sdk",
    '-isystem', (Join-Path $sdk 'target\include'),
    '-L', (Join-Path $sdk 'target\lib'),
    '-L', (Join-Path $sdk 'target\user\homebrew\lib'),
    '-fno-stack-protector', '-fno-plt', '-femulated-tls',
    '-nostartfiles'
  ) + $includeFlags + $cflags + $sdkDefines + @(
    '-Wall', '-Wextra', '-Wno-unused-parameter'
  ) + $sources + @(
    '-lc', '-lkernel_web',
    '--end-no-unused-arguments',
    (Join-Path $sdk 'target\lib\crt1.o'),
    $platformLibraries,
    '-o', $Output
  )
}

Write-Host "sdk    : $sdk"
Write-Host "clang  : $clang"
Write-Host "config : $Config"
& $clang @argsList
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
$elfHeader = [System.IO.File]::ReadAllBytes($Output)
if ($elfHeader.Length -lt 16 -or $elfHeader[0] -ne 0x7f -or
    $elfHeader[1] -ne 0x45 -or $elfHeader[2] -ne 0x4c -or
    $elfHeader[3] -ne 0x46 -or $elfHeader[7] -ne 9) {
  throw 'PS5 build rejected: ELF OS/ABI must be FreeBSD (9).'
}

Get-Item $Output | Select-Object FullName, Length
