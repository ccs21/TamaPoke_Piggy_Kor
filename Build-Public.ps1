param(
    [switch]$SkipFirmware,
    [switch]$SkipFlasher,
    [switch]$KeepBuildCache
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$firmware = Join-Path $root 'firmware'
$flasher = Join-Path $root 'flasher'
$payload = Join-Path $flasher 'payload'
$cache = Join-Path $root '_build'
$release = Join-Path $root 'release'

function Assert-ChildPath([string]$Path, [string]$Parent) {
    $full = [IO.Path]::GetFullPath($Path)
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $full.StartsWith($parentFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped its expected parent: $full"
    }
    return $full
}

function Reset-ChildDirectory([string]$Path, [string]$Parent) {
    $safe = Assert-ChildPath $Path $Parent
    if (Test-Path -LiteralPath $safe) { Remove-Item -LiteralPath $safe -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $safe | Out-Null
}

function Find-Tool([string]$Name, [string]$SearchRoot) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $found = Get-ChildItem -LiteralPath $SearchRoot -Recurse -Filter "$Name.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (-not $found) { throw "$Name was not found." }
    return $found
}

if (-not (Test-Path -LiteralPath $firmware)) { throw "Firmware source is missing: $firmware" }
New-Item -ItemType Directory -Force -Path $cache,$release,$payload | Out-Null

$arduinoRoot = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32'
$arduino = Find-Tool 'arduino-cli' (Join-Path $env:LOCALAPPDATA 'Arduino15')
if (-not $SkipFirmware) {
    Reset-ChildDirectory $payload $flasher

    $targets = @(
        [pscustomobject]@{ Label='1.75'; Is175C=0; FlashMenu='16M'; Partition='partitions_175.csv' },
        [pscustomobject]@{ Label='1.75C'; Is175C=1; FlashMenu='32M'; Partition='partitions_175c.csv' }
    )
    foreach ($target in $targets) {
        $stageParent = Join-Path $cache "stage-$($target.Label)"
        $stage = Join-Path $stageParent 'TamaPoke_Final'
        $build = Join-Path $cache "arduino-$($target.Label)"
        Reset-ChildDirectory $stageParent $cache
        Reset-ChildDirectory $build $cache
        New-Item -ItemType Directory -Force -Path $stage | Out-Null
        Get-ChildItem -LiteralPath $firmware -File |
            Where-Object { $_.Extension -in @('.ino','.cpp','.h') } |
            Copy-Item -Destination $stage
        $generatedStage = Join-Path $stage 'generated'
        New-Item -ItemType Directory -Force -Path $generatedStage | Out-Null
        foreach ($generatedName in @(
            'dex_names_ko.h','help_ko.h','i18n_strings_ko.h',
            'korean_font_data.h','manual_qr.h')) {
            Copy-Item -LiteralPath (Join-Path $firmware "generated\$generatedName") -Destination $generatedStage
        }
        Copy-Item -LiteralPath (Join-Path $firmware $target.Partition) -Destination (Join-Path $stage 'partitions.csv')
        $fqbn = "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=$($target.FlashMenu),PartitionScheme=custom,PSRAM=opi,EraseFlash=none"
        & $arduino compile --jobs 2 --fqbn $fqbn `
            --build-property "compiler.cpp.extra_flags=-DTAMAPOKE_BOARD_175C=$($target.Is175C)" `
            --build-path $build $stage
        if ($LASTEXITCODE -ne 0) { throw "$($target.Label) firmware compilation failed." }
        $stem = 'TamaPoke_Final.ino'
        $map = @{
            'app.bin' = (Join-Path $build "$stem.bin")
            'bootloader.bin' = (Join-Path $build "$stem.bootloader.bin")
            'partitions.bin' = (Join-Path $build "$stem.partitions.bin")
            'boot_app0.bin' = (Join-Path $build 'boot_app0.bin')
        }
        foreach ($suffix in $map.Keys) {
            if (-not (Test-Path -LiteralPath $map[$suffix])) { throw "Missing output: $($map[$suffix])" }
            Copy-Item -LiteralPath $map[$suffix] -Destination (Join-Path $payload "base-$($target.Label)-$suffix") -Force
        }
    }
}

if (-not $SkipFlasher) {
    $publish = Join-Path $cache 'flasher-publish'
    Reset-ChildDirectory $publish $cache
    & dotnet publish (Join-Path $flasher 'TamaPokeFlasher.csproj') -c Release -r win-x64 `
        --self-contained true -p:PublishSingleFile=true -p:EnableCompressionInSingleFile=true `
        -p:IncludeNativeLibrariesForSelfExtract=true -o $publish
    if ($LASTEXITCODE -ne 0) { throw 'Public flasher publish failed.' }

    $package = Join-Path $release 'TamaPoke-Korean-Public-Flasher'
    Reset-ChildDirectory $package $release
    Copy-Item -LiteralPath (Join-Path $publish 'TamaPokeFlasher.exe') -Destination (Join-Path $package 'TamaPoke-Flasher.exe')
    Copy-Item -LiteralPath (Join-Path $root 'FLASHER-GUIDE.txt') -Destination $package
    foreach ($notice in @('LICENSE','CREDITS.md','THIRD_PARTY_NOTICES.md')) {
        Copy-Item -LiteralPath (Join-Path $root $notice) -Destination $package
    }
    $packageDocs = Join-Path $package 'docs'
    $packageLicenses = Join-Path $package 'licenses'
    New-Item -ItemType Directory -Force -Path $packageDocs,$packageLicenses | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'docs\ADDITIONAL_ASSETS_GUIDE.md') -Destination $packageDocs
    Copy-Item -LiteralPath (Join-Path $root 'docs\ASSET_LAYOUT.svg') -Destination $packageDocs
    Copy-Item -LiteralPath (Join-Path $root 'docs\LEGAL.md') -Destination $packageDocs
    Copy-Item -LiteralPath (Join-Path $root 'licenses\NotoSansKR-OFL.txt') -Destination $packageLicenses
    Copy-Item -LiteralPath (Join-Path $root 'licenses\Arduino-ESP32-LICENSE.md') -Destination $packageLicenses

    $dotnetRoot = Split-Path -Parent (Get-Command dotnet).Source
    $dotnetLicense = Join-Path $dotnetRoot 'LICENSE.txt'
    $dotnetNotices = Join-Path $dotnetRoot 'ThirdPartyNotices.txt'
    if (-not (Test-Path -LiteralPath $dotnetLicense) -or -not (Test-Path -LiteralPath $dotnetNotices)) {
        throw 'The .NET license or third-party notice file was not found beside dotnet.exe.'
    }
    Copy-Item -LiteralPath $dotnetLicense -Destination (Join-Path $package 'DOTNET-LICENSE.txt')
    Copy-Item -LiteralPath $dotnetNotices -Destination (Join-Path $package 'DOTNET-THIRD-PARTY-NOTICES.txt')
}

if (-not $KeepBuildCache) {
    $safeCache = Assert-ChildPath $cache $root
    if (Test-Path -LiteralPath $safeCache) { Remove-Item -LiteralPath $safeCache -Recurse -Force }
}

Write-Host "Public flasher build complete: $release"
