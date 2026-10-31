#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Development,
    [ValidateNotNullOrEmpty()][string]$BuildDirectory = '.local/public-build',
    [ValidateNotNullOrEmpty()][string]$MetContactConfig
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$python = $null
foreach ($name in @('python', 'python3')) {
    $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command -and $command.Path -notlike '*WindowsApps*') { $python = $command.Path; break }
}
if (-not $python) { throw 'Python 3.10+ was not found. Activate ESP-IDF v6.1; see docs/building.md.' }
function Get-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}
function Get-ShortCompilerBin([string]$ProjectRoot, [string]$CompilerPath) {
    $compilerRoot = [IO.Path]::GetFullPath((Split-Path (Split-Path $CompilerPath -Parent) -Parent))
    foreach ($name in @('riscv32-esp-elf-gcc.exe', 'riscv32-esp-elf-g++.exe')) {
        if (-not (Test-Path -LiteralPath (Join-Path $compilerRoot "bin/$name") -PathType Leaf)) {
            throw 'The activated RISC-V compiler root is incomplete.'
        }
    }
    $shortRoot = Join-Path $ProjectRoot '.local/tc'
    $existing = Get-Item -LiteralPath $shortRoot -Force -ErrorAction SilentlyContinue
    if ($existing) {
        $target = @($existing.Target) | Select-Object -First 1
        if ($existing.LinkType -ne 'Junction' -or -not $target -or
            [IO.Path]::GetFullPath($target) -ne $compilerRoot) {
            throw 'The project .local/tc conflicts with this SDK. It was left unchanged; use another checkout or resolve it explicitly.'
        }
    } else {
        New-Item -ItemType Directory -Force -Path (Split-Path $shortRoot -Parent) | Out-Null
        New-Item -ItemType Junction -Path $shortRoot -Target $compilerRoot | Out-Null
    }
    return Join-Path $shortRoot 'bin'
}
$buildPath = Get-ProjectPath $BuildDirectory
$contactPath = ''
if ($PSBoundParameters.ContainsKey('MetContactConfig')) { $contactPath = Get-ProjectPath $MetContactConfig }
$doctorArguments = @((Join-Path $PSScriptRoot 'check-build.py'), '--build-directory', $buildPath)
if ($contactPath) { $doctorArguments += @('--met-contact-config', $contactPath) }
& $python @doctorArguments
if ($LASTEXITCODE -ne 0) { throw 'Build prerequisites are incomplete. See the checks above.' }
if ($Check) { return }

$configPath = Join-Path $buildPath 'sdkconfig'
$lockPath = Join-Path $buildPath 'dependencies.lock'
$descriptionPath = Join-Path $buildPath 'project_description.json'
if (Test-Path -LiteralPath $descriptionPath) {
    $description = Get-Content -LiteralPath $descriptionPath -Raw | ConvertFrom-Json
    if ([IO.Path]::GetFullPath($description.config_file) -ne $configPath) {
        throw 'This directory uses another sdkconfig. Choose a fresh -BuildDirectory under .local.'
    }
}
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
if (Test-Path -LiteralPath $configPath) { $text = [IO.File]::ReadAllText($configPath) }
else { $text = [IO.File]::ReadAllText((Join-Path $projectRoot 'firmware/sdkconfig')) }
$pattern = '(?m)^(CONFIG_CLOCK_DIAGNOSTICS=[^\r\n]*|# CONFIG_CLOCK_DIAGNOSTICS is not set)(?=\r?$)'
if ([regex]::Matches($text, $pattern).Count -ne 1) { throw 'Expected exactly one CLOCK_DIAGNOSTICS setting in sdkconfig.' }
$setting = '# CONFIG_CLOCK_DIAGNOSTICS is not set'
$defaults = 'sdkconfig.defaults'
if ($Development) { $setting = 'CONFIG_CLOCK_DIAGNOSTICS=y'; $defaults += ';sdkconfig.development' }
$text = [regex]::Replace($text, $pattern, $setting)
[IO.File]::WriteAllText($configPath, $text, (New-Object Text.UTF8Encoding($false)))

# A child-specific environment preserves an explicitly empty contact setting on Windows.
# The caller's environment and the tracked config/lock remain unchanged.
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $python
$start.UseShellExecute = $false
$start.WorkingDirectory = $projectRoot
$start.EnvironmentVariables['IDF_COMPONENT_CONSTRAINT_FILES'] = Join-Path $projectRoot 'firmware/dependency-constraints.txt'
$start.EnvironmentVariables.Remove('IDF_COMPONENT_CONSTRAINTS')
$start.EnvironmentVariables['HOME_CLOCK_MET_CONTACT_CONFIG'] = $contactPath
if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $compiler = (Get-Command riscv32-esp-elf-gcc -CommandType Application | Select-Object -First 1).Path
    $shortBin = Get-ShortCompilerBin $projectRoot $compiler
    if ((Test-Path -LiteralPath $descriptionPath) -and $description.c_compiler -and
        [IO.Path]::GetFullPath($description.c_compiler) -ne (Join-Path $shortBin 'riscv32-esp-elf-gcc.exe')) {
        throw 'This build cache uses another compiler path. Choose a fresh -BuildDirectory under .local.'
    }
    $start.EnvironmentVariables['PATH'] = $shortBin + [IO.Path]::PathSeparator + $start.EnvironmentVariables['PATH']
    Write-Host "Windows compiler root: $shortBin (SDK: $(Split-Path (Split-Path $compiler -Parent) -Parent))"
}

$idfArguments = @((Join-Path $env:IDF_PATH 'tools/idf.py'), '-C', (Join-Path $projectRoot 'firmware'),
    '-B', $buildPath, '-D', 'IDF_TARGET=esp32p4', '-D', "SDKCONFIG=$configPath",
    '-D', "CLOCK_DEPENDENCIES_LOCK=$lockPath", '-D', "SDKCONFIG_DEFAULTS=$defaults", 'reconfigure', 'build')
if ($start.PSObject.Properties['ArgumentList']) {
    foreach ($argument in $idfArguments) { $start.ArgumentList.Add($argument) }
} else {
    # The preflight disallows whitespace/quotes in project, output and SDK paths.
    if ($idfArguments | Where-Object { $_ -match '[\s"]' }) { throw 'Use PowerShell 7 or paths without spaces/quotes.' }
    $start.Arguments = $idfArguments -join ' '
}
Write-Host ('Building ESP32-P4, diagnostics=' + [bool]$Development + ', MET contact=' + [bool]$contactPath)
$process = [Diagnostics.Process]::Start($start)
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "ESP-IDF build failed (exit $($process.ExitCode))." }
$header = [IO.File]::ReadAllText((Join-Path $buildPath 'config/sdkconfig.h'))
$diagnosticsEnabled = [regex]::IsMatch($header, '(?m)^#define CONFIG_CLOCK_DIAGNOSTICS\b')
if ($diagnosticsEnabled -ne [bool]$Development -or $header -notmatch '#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200') {
    throw 'The generated diagnostic/console configuration does not match this build mode.'
}
Write-Host "Built: $buildPath/home_clock.bin"
Write-Host 'No device was opened or flashed.'
