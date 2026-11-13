# Starts the installed NuWorld app, waits, takes a screen shot, stops it (unless -Keep) and prints the log end.
#   powershell -File xbox\launch_pc.ps1 [-Seconds 30] [-Shot <png>] [-Keep]
param([int]$Seconds = 30, [string]$Shot = "", [switch]$Keep)
$pfn = (Get-AppxPackage NuWorld.CrashWOC).PackageFamilyName
$root = Join-Path $env:LOCALAPPDATA "Packages\$pfn\LocalState\crashwoc-nuworld"
Get-Process NuWorld -ErrorAction SilentlyContinue | Stop-Process -Force
# The first activation after a (re)registration can fail with "The app didn't start": try up to three times.
for ($try = 0; $try -lt 3; $try++) {
    Start-Process "shell:AppsFolder\$pfn!App" -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 4
    if (Get-Process NuWorld -ErrorAction SilentlyContinue) { break }
}
Write-Host "started: $([bool](Get-Process NuWorld -ErrorAction SilentlyContinue)) (tries $($try + 1))"
Start-Sleep -Seconds $Seconds
Write-Host "alive after ${Seconds}s: $([bool](Get-Process NuWorld -ErrorAction SilentlyContinue))"
if ($Shot -ne "") {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $bmp.Save($Shot)
    Write-Host "shot $Shot"
}
if (-not $Keep) { Get-Process NuWorld -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1 }
$log = Join-Path $root "nuworld.log"
if (-not $Keep -and (Test-Path $log)) { Get-Content $log -Tail 40 }
