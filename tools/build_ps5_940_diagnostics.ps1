# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SdkPath, [switch]$WithCollector,
  [ValidateSet(720,1080,1440,2160)][int]$Height=1080,
  [ValidateSet(60,120)][int]$CandidateFps=60)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sdk = (Resolve-Path -LiteralPath $SdkPath).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'target/include/ps5/kernel.h'))) {
  throw 'PS5 Payload SDK headers not found.'
}
$compiler = (Get-Command clang -ErrorAction Stop).Source
$directory = if ($CandidateFps -eq 120) { 'payload/build/diagnostics-940-120fps' } else { 'payload/build/diagnostics-940' }
$outDir = Join-Path $repo $directory
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
# This builder never invokes the standard build or writes its ELF/assets.
$standard = Join-Path $repo 'payload/build/pba.elf'
$before = if (Test-Path -LiteralPath $standard) { (Get-FileHash -LiteralPath $standard).Hash } else { 'absent' }
$argsBase = @('--start-no-unused-arguments', '-target', 'x86_64-sie-ps5',
  "--sysroot=$sdk", '-isystem', (Join-Path $sdk 'target/include'),
  '-L', (Join-Path $sdk 'target/lib'), '-L', (Join-Path $sdk 'target/user/homebrew/lib'),
  '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-nostartfiles',
  '-I', (Join-Path $repo 'payload/src'), '-O2', '-g', '-Wall', '-Wextra',
  '-Werror', '-Wno-unused-parameter')
$sources = @((Join-Path $PSScriptRoot 'diagnostics/ps5_940_main.c'),
  (Join-Path $repo 'payload/src/common/process/inject.c'),
  (Join-Path $repo 'payload/src/platform/ps5/offset/940.c'))
$tail = @('-lc', '-lkernel_web', '--end-no-unused-arguments',
  (Join-Path $sdk 'target/lib/crt1.o'), '-lSceLibcInternal')
$oldPath = $env:PATH
try {
  $env:PATH = (Join-Path $sdk 'win') + ';' + $oldPath
  $manifest = @()
  foreach ($capture in @(0,1)) {
    $name = if ($capture) { 'pbs-ps5-940-capture-1080p60-10s.elf' } else { 'pbs-ps5-940-open-only.elf' }
    if ($CandidateFps -eq 120 -or $Height -ne 1080) {
      $kind = if ($capture) { 'capture-10s' } else { 'open-only' }
      $name = "pbs-ps5-940-$kind-${Height}p-candidate${CandidateFps}.elf"
    }
    $output = Join-Path $outDir $name
    $log = Join-Path $outDir ($name + '.build.log')
    $enum = if ($CandidateFps -eq 120) { 2 } else { 1 }
    $compilerArgs = $argsBase + @("-DPBS_940_CAPTURE=$capture", "-DPBS_940_HEIGHT=$Height", "-DPBS_940_FPS_MODE=$enum") + $sources + $tail + @('-o',$output)
    "compiler=$compiler`nsdk=$sdk`narguments=$($compilerArgs -join ' ')" | Set-Content -LiteralPath $log
    & $compiler @compilerArgs 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic build failed: $name" }
    & python (Join-Path $PSScriptRoot 'verify_ps5_elf.py') $output 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw "Diagnostic ELF validation failed: $name" }
    $manifest += [ordered]@{file=$name; bytes=(Get-Item -LiteralPath $output).Length;
      sha256=(Get-FileHash -LiteralPath $output).Hash; capture=[bool]$capture}
  }
  if ($WithCollector) {
    $name = 'pbs-ps5-940-collect.elf'
    $output = Join-Path $outDir $name
    $log = Join-Path $outDir ($name + '.build.log')
    $collectorArgs = $argsBase + @((Join-Path $PSScriptRoot 'diagnostics/ps5_940_collect.c')) +
      $tail + @('-lSceNet','-o',$output)
    "compiler=$compiler`nsdk=$sdk`narguments=$($collectorArgs -join ' ')" | Set-Content -LiteralPath $log
    & $compiler @collectorArgs 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw 'Read-only collector build failed.' }
    & python (Join-Path $PSScriptRoot 'verify_ps5_elf.py') $output 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw 'Read-only collector ELF validation failed.' }
    $manifest += [ordered]@{file=$name; bytes=(Get-Item -LiteralPath $output).Length;
      sha256=(Get-FileHash -LiteralPath $output).Hash; kind='read-only collector'}
  }
  $after = if (Test-Path -LiteralPath $standard) { (Get-FileHash -LiteralPath $standard).Hash } else { 'absent' }
  if ($before -ne $after) { throw 'Standard pba.elf changed unexpectedly.' }
  [ordered]@{firmware='0x09400008'; height=$Height; candidate_fps=$CandidateFps; artifacts=$manifest; standard_pba_sha256=$after;
    standard_unchanged=$true; hardware_tested=$false} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outDir 'manifest.json')
  $manifest | Format-Table
} finally { $env:PATH = $oldPath }
