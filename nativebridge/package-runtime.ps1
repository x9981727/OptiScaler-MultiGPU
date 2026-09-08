param(
    [Parameter(Mandatory=$true)][string]$DistRoot,
    [Parameter(Mandatory=$true)][string]$OptiScalerOutput
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

$runtime=Join-Path $DistRoot 'NativeBridge-Runtime-x64'
if(Test-Path $runtime){Remove-Item $runtime -Recurse -Force}
New-Item -ItemType Directory -Force $runtime | Out-Null

$magpieSource=Join-Path $DistRoot 'Magpie-NativeBridge-x64'
if(!(Test-Path (Join-Path $magpieSource 'Magpie.exe'))){throw 'Magpie runtime output missing'}
Copy-Item $magpieSource (Join-Path $runtime 'Magpie') -Recurse

$producer=Join-Path $runtime 'OptiScaler-Producer'
New-Item -ItemType Directory -Force $producer | Out-Null
if(!(Test-Path $OptiScalerOutput)){throw 'OptiScaler output directory missing'}
$allowed=@('.dll','.ini','.json','.bat','.config','.txt')
Get-ChildItem $OptiScalerOutput -Recurse -File | Where-Object {$allowed -contains $_.Extension.ToLowerInvariant()} | ForEach-Object {
    $relative=$_.FullName.Substring($OptiScalerOutput.Length).TrimStart('\\','/')
    $target=Join-Path $producer $relative
    New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
    Copy-Item $_.FullName $target -Force
}
if(!(Get-ChildItem $producer -Recurse -File | Where-Object {$_.Name -ieq 'OptiScaler.dll'})){
    throw 'Packaged OptiScaler.dll missing'
}

$launcher=@'
param(
    [Parameter(Mandatory=$true)][string]$GameExe,
    [string]$GameArgs = ''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $MyInvocation.MyCommand.Path
$magpie=Join-Path $root 'Magpie\Magpie.exe'
if(!(Test-Path $GameExe)){throw "Game executable not found: $GameExe"}
if(!(Test-Path $magpie)){throw "Magpie.exe not found: $magpie"}
$env:MAGPIE_NATIVE_BRIDGE='1'
Write-Host 'NativeBridge enabled for child processes.' -ForegroundColor Green
Write-Host 'Starting game first; it must already have the patched OptiScaler producer installed using the game-appropriate OptiScaler proxy method.'
$game=Start-Process -FilePath $GameExe -ArgumentList $GameArgs -PassThru
Start-Sleep -Seconds 2
$magpieProc=Start-Process -FilePath $magpie -WorkingDirectory (Split-Path $magpie -Parent) -PassThru
Write-Host "Game PID: $($game.Id)"
Write-Host "Magpie PID: $($magpieProc.Id)"
Write-Host 'In Magpie, explicitly select the NVIDIA secondary GPU. Keep the game on the AMD primary GPU.'
'@
[IO.File]::WriteAllText((Join-Path $runtime 'Launch-NativeBridge.ps1'),$launcher,[Text.UTF8Encoding]::new($false))

$readme=@'
# Magpie NativeBridge Runtime x64

## Purpose
Game renders on the primary AMD GPU. Patched OptiScaler captures the game-provided Streamline color/depth/motion/camera data. NativeBridge transfers the synchronized frame guidance to Magpie on the NVIDIA secondary GPU for frame-generation processing.

## Important safety / compatibility notes
- Offline or single-player testing only. Do not use with online/anti-cheat protected games.
- OptiScaler installation/proxy filename is game-specific. This package intentionally does NOT auto-rename OptiScaler.dll to dxgi.dll/winmm.dll/etc. Install the producer using the official OptiScaler compatibility instructions for the target game.
- The game must expose compatible Streamline resources. Unsupported resource formats fail closed rather than fabricating depth/camera data.
- Real AMD + NVIDIA dual-GPU gameplay validation is still required; Windows CI proves build, IPC, shared-resource interop and SDK linkage, not your exact hardware/game.

## Test procedure
1. Install the files from `OptiScaler-Producer` into the target game using that game's documented OptiScaler installation method.
2. Keep the game configured to render on the AMD primary GPU.
3. Run PowerShell:
   `./Launch-NativeBridge.ps1 -GameExe 'C:\Path\Game.exe'`
4. In Magpie, select the NVIDIA secondary GPU as the processing/graphics adapter.
5. Start with SDR and 2x frame generation. Confirm logs show NativeBridge producer/consumer connection and native depth/motion/camera guidance.
6. Only after stable operation, test HDR / higher frame-generation multipliers.

## Build validation
The package is generated only after the CI gate successfully builds:
- NativeBridge protocol/IPC/D3D12 transport tests
- Magpie x64 with the real Intel XeSS-FG SDK branch compiled and linked
- Native Depth + raw Motion + Camera guidance for XeSS-FG
- Native Depth/Motion/Camera plus exact motion normalization and camera position for DLSSFG
- patched OptiScaler x64 producer

AMD + NVIDIA real-machine gameplay remains a separate validation step.
'@
[IO.File]::WriteAllText((Join-Path $runtime 'README_繁體中文說明.md'),$readme,[Text.UTF8Encoding]::new($false))

$manifest=[ordered]@{
    generatedUtc=(Get-Date).ToUniversalTime().ToString('o')
    architecture='x64'
    nativeBridgeEnabled='MAGPIE_NATIVE_BRIDGE=1'
    magpieExe='Magpie/Magpie.exe'
    optiScalerProducer='OptiScaler-Producer/OptiScaler.dll'
    ciValidated=$true
    realAmdNvidiaGameValidated=$false
    sha256=@{}
}
Get-ChildItem $runtime -Recurse -File | ForEach-Object {
    $relative=$_.FullName.Substring($runtime.Length).TrimStart('\\','/').Replace('\\','/')
    $manifest.sha256[$relative]=(Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $runtime 'manifest.json') -Encoding UTF8

$zip=Join-Path $DistRoot 'Magpie-NativeBridge-Runtime-x64.zip'
if(Test-Path $zip){Remove-Item $zip -Force}
Compress-Archive -Path (Join-Path $runtime '*') -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash $zip -Algorithm SHA256 | Format-List | Out-String | Set-Content (Join-Path $DistRoot 'Magpie-NativeBridge-Runtime-x64.sha256.txt')
Write-Host "Runtime package: $zip"
