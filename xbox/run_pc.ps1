# Runs the packed layout as a real UWP (AppContainer) app on this PC, the way the Xbox runs it.
#   powershell -File xbox\run_pc.ps1 -Layout <out>\pkg [-Data <folder with disc\>] [-ArgsFile <args.txt>] [-Seconds 60]
# Registers the loose layout (needs Windows Developer Mode), seeds LocalState\crashwoc-nuworld, starts the app,
# waits, prints the end of nuworld.log and stops the app.
param(
    [Parameter(Mandatory = $true)][string]$Layout,
    [string]$Data = "",
    [string]$ArgsFile = "",
    [int]$Seconds = 60,
    [switch]$Keep
)
$ErrorActionPreference = "Stop"
$name = "NuWorld.CrashWOC"
Get-Process NuWorld -ErrorAction SilentlyContinue | Stop-Process -Force
Add-AppxPackage -Register (Join-Path $Layout "AppxManifest.xml") -ForceUpdateFromAnyVersion -ForceApplicationShutdown
$pkg = Get-AppxPackage $name
$pfn = $pkg.PackageFamilyName
$root = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState\crashwoc-nuworld"
New-Item -ItemType Directory -Force $root | Out-Null
if ($Data -ne "" -and -not (Test-Path (Join-Path $root "disc"))) {
    Write-Host "seeding $root\disc from $Data"
    robocopy (Join-Path $Data "disc") (Join-Path $root "disc") /E /NFL /NDL /NJH /NJS /NP | Out-Null
}
$argsTarget = Join-Path $root "args.txt"
if ($ArgsFile -ne "") { Copy-Item $ArgsFile $argsTarget -Force } elseif (Test-Path $argsTarget) { Remove-Item $argsTarget }
$log = Join-Path $root "nuworld.log"
if (Test-Path $log) { Remove-Item $log }
Write-Host "starting $pfn!App"
Start-Process "shell:AppsFolder\$pfn!App" -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if (-not (Get-Process NuWorld -ErrorAction SilentlyContinue) -and (Get-Date) -gt (Get-Date).AddSeconds(-$Seconds + 10)) {
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern "exit code" -Quiet)) { break }
    }
}
$alive = [bool](Get-Process NuWorld -ErrorAction SilentlyContinue)
Write-Host "process alive at the end: $alive"
if (-not $Keep) { Get-Process NuWorld -ErrorAction SilentlyContinue | Stop-Process -Force }
Write-Host "== $log"
if (Test-Path $log) { Get-Content $log -Tail 60 } else { Write-Host "(no log)" }
