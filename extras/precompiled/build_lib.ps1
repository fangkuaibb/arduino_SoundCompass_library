# Rebuild precompiled libsl_core.a for every supported ESP32 target
#
# 支持的芯片（同时满足 I2S TDM + esp-sr 提供 VAD 预编译库）：
#   esp32s3 / esp32c3 / esp32c5 / esp32c6 / esp32p4
# 不支持的芯片：
#   esp32/esp32s2/esp32c2 无 I2S TDM；esp32h2 esp-sr 未提供预编译库
#
# 流程（每个目标独立执行）：
#   1. 用对应架构的工具链编译 sl_core.c
#   2. 用该架构的 ld -r 把 sl_core.o 与 esp-sr 归档里“实际需要”的 VAD 目标文件合并
#      （静态链接语义，自动包含传递依赖，换芯片不用手工维护成员列表）
#   3. 打包成 <library>/src/<target>/libsl_core.a
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build_lib.ps1
# Optional:
#   -Targets   逗号分隔的目标列表，默认全部
#   -XtensaBin xtensa 工具链 bin 目录
#   -RiscvBin  riscv32 工具链 bin 目录
#   -EspSrDir  esp-sr 组件目录（含 lib/<target>/libesp_audio_processor.a）
param(
    [string]$Targets = "esp32s3,esp32c3,esp32c5,esp32c6,esp32p4",
    [string]$XtensaBin = "C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin",
    [string]$RiscvBin = "C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin",
    [string]$EspSrDir = ""
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$libRoot = Split-Path -Parent (Split-Path -Parent $scriptDir)
if ([string]::IsNullOrWhiteSpace($EspSrDir)) {
    $repoRoot = Split-Path -Parent (Split-Path -Parent $libRoot)
    $EspSrDir = Join-Path $repoRoot "managed_components\espressif__esp-sr"
}

# 每个目标的工具链前缀和架构相关编译参数（与 ESP-IDF v5.5 的 soc/project_include.cmake 保持一致）
$targetInfo = @{
    "esp32s3" = @{ Prefix = "xtensa-esp32s3-elf"; Bin = $XtensaBin; Arch = @("-mlongcalls", "-mtext-section-literals") }
    "esp32c3" = @{ Prefix = "riscv32-esp-elf";    Bin = $RiscvBin;  Arch = @("-march=rv32imc_zicsr_zifencei",  "-mabi=ilp32") }
    "esp32c5" = @{ Prefix = "riscv32-esp-elf";    Bin = $RiscvBin;  Arch = @("-march=rv32imac_zicsr_zifencei", "-mabi=ilp32") }
    "esp32c6" = @{ Prefix = "riscv32-esp-elf";    Bin = $RiscvBin;  Arch = @("-march=rv32imac_zicsr_zifencei", "-mabi=ilp32") }
    "esp32p4" = @{ Prefix = "riscv32-esp-elf";    Bin = $RiscvBin;  Arch = @("-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f") }
}

$commonFlags = @(
    "-Os", "-ffunction-sections", "-fdata-sections", "-fno-common",
    "-std=gnu17", "-Wall", "-Wextra",
    "-I", (Join-Path $libRoot "src")
)

foreach ($target in ($Targets -split ",")) {
    $target = $target.Trim()
    if ([string]::IsNullOrWhiteSpace($target)) { continue }
    if (-not $targetInfo.ContainsKey($target)) {
        throw "unsupported target: $target (supported: $($targetInfo.Keys -join ', '))"
    }

    $info = $targetInfo[$target]
    $gcc = Join-Path $info.Bin "$($info.Prefix)-gcc.exe"
    $ld = Join-Path $info.Bin "$($info.Prefix)-ld.exe"
    $ar = Join-Path $info.Bin "$($info.Prefix)-ar.exe"
    $nm = Join-Path $info.Bin "$($info.Prefix)-nm.exe"
    $inputLib = Join-Path $EspSrDir "lib\$target\libesp_audio_processor.a"
    $outDir = Join-Path $libRoot "src\$target"
    $outLib = Join-Path $outDir "libsl_core.a"

    foreach ($f in @($gcc, $ld, $ar, $nm, $inputLib)) {
        if (-not (Test-Path $f)) { throw "missing file: $f" }
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $tmp = Join-Path $env:TEMP ("sl_core_" + $target + "_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        Write-Host "[$target] compile sl_core.c"
        $cflags = $commonFlags + $info.Arch + @(
            "-I", (Join-Path $EspSrDir "include\$target"),
            "-c", (Join-Path $scriptDir "sl_core.c"),
            "-o", (Join-Path $tmp "sl_core.o")
        )
        & $gcc @cflags
        if ($LASTEXITCODE -ne 0) { throw "[$target] failed to compile sl_core.c" }

        Write-Host "[$target] collect VAD objects from esp-sr"
        $combined = Join-Path $tmp "sl_core_full.o"
        & $ld -r -o $combined (Join-Path $tmp "sl_core.o") $inputLib
        if ($LASTEXITCODE -ne 0) { throw "[$target] failed to combine with esp-sr archive" }

        $defined = & $nm --defined-only $combined
        foreach ($sym in @("vad_create_with_param", "vad_process", "vad_destroy")) {
            if (-not ($defined | Select-String -SimpleMatch $sym)) {
                throw "[$target] symbol not found: $sym"
            }
        }

        Write-Host "[$target] archive $outLib"
        if (Test-Path $outLib) { Remove-Item $outLib -Force }
        & $ar rcs $outLib $combined
        if ($LASTEXITCODE -ne 0) { throw "[$target] ar failed" }

        $undef = (& $nm -u $combined | ForEach-Object { ($_ -split '\s+')[-1] }) | Where-Object { $_ }
        $size = (Get-Item $outLib).Length / 1KB
        Write-Host ("[{0}] done: {1:N1} KB, external symbols: {2}" -f $target, $size, ($undef -join ", "))
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
