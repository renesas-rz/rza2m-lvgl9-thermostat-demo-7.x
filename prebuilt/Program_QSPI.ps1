# Program RZ/A2M EVK QSPI flash with SEGGER J-Link (Windows)
#
#   Program_QSPI.bat              asks which image: all, uboot, dtb, kernel or rootfs
#   .\Program_QSPI.ps1 uboot      that image, no question
#
# Images are taken from this folder, else from ..\output\images.
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dir = $Here
if (-not (Test-Path (Join-Path $Dir "xipImage.bin"))) { $Dir = Join-Path $Here "..\output\images" }

$JLink = $null
$reg = Get-ItemProperty "HKCU:\Software\SEGGER\J-Link" -ErrorAction SilentlyContinue
if ($reg -and $reg.InstallPath -and (Test-Path (Join-Path $reg.InstallPath "JLink.exe"))) { $JLink = Join-Path $reg.InstallPath "JLink.exe" }
if (-not $JLink) {
  $found = Get-ChildItem "C:\Program Files\SEGGER\JLink*\JLink.exe" -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
  if ($found) { $JLink = $found.FullName }
}
if (-not $JLink) { Write-Host "JLink.exe not found: install the SEGGER J-Link software" -ForegroundColor Red; exit 1 }

$Table = @{
  "uboot"  = @("u-boot.bin",               "0x20000000")
  "dtb"    = @("r7s9210-rza2mevb.dtb.bin", "0x200C0000")
  "kernel" = @("xipImage.bin",             "0x20200000")
  "rootfs" = @("rootfs.cramfs.bin",        "0x20800000")
}
$choice = if ($args.Count -gt 0) { $args[0] } else { (Read-Host "Flash which image? (all, uboot, dtb, kernel, rootfs)").Trim() }
if ($choice -eq "all") {
  $Images = @($Table["uboot"], $Table["dtb"], $Table["kernel"], $Table["rootfs"])
} elseif ($Table.ContainsKey($choice)) {
  $Images = @(,$Table[$choice])
} else {
  Write-Host "Choose one of: all uboot dtb kernel rootfs" -ForegroundColor Red; exit 1
}

$lines = @("rx 100")
foreach ($i in $Images) {
  $f = Join-Path $Dir $i[0]
  if (-not (Test-Path $f)) { Write-Host "Missing $f" -ForegroundColor Red; exit 1 }
  Write-Host ("{0} -> {1}" -f $i[0], $i[1])
  $lines += "loadbin $($i[0]),$($i[1])"
  $lines += "verifybin $($i[0]),$($i[1])"
}
$lines += "exit"
$cmd = Join-Path $env:TEMP "rza2m_flash.jlink"
Set-Content -Path $cmd -Value $lines -Encoding ascii

Write-Host ""
Write-Host "Before flashing:" -ForegroundColor Yellow
Write-Host "  SUB SW6 program mode: 1 OFF, 2 ON, 3 OFF, 4 ON, 5 ON, 6 ON" -ForegroundColor Yellow
Write-Host "  J-Link on CN5, power on, stop in U-Boot (press a key in the terminal)" -ForegroundColor Yellow
Read-Host "Press Enter to flash (Ctrl+C to cancel)" | Out-Null

$log = Join-Path $Dir "flash.log"
$start = Get-Date
Push-Location $Dir
& $JLink -device R7S921053VCBG -if JTAG -speed 15000 -jtagconf -1,-1 -autoconnect 1 -CommanderScript $cmd | Tee-Object -FilePath $log
Pop-Location
Remove-Item $cmd -ErrorAction SilentlyContinue

$ok = @(Select-String -Path $log -Pattern "Verify successful").Count
$secs = [int]((Get-Date) - $start).TotalSeconds
Write-Host ""
Write-Host "Verified $ok of $($Images.Count) images in $secs s (log: $log)"
if ($ok -eq $Images.Count) {
  Write-Host "DONE. Power off, set SUB SW6 run mode: 1 OFF, 2 OFF, 3 OFF, 4 ON, 5 ON, 6 ON, 7 ON, 8 OFF, 9 OFF, 10 OFF" -ForegroundColor Green
  Write-Host "Power on: U-Boot boots Linux after 3 s (press a key to stop at the prompt)" -ForegroundColor Green
} else {
  Write-Host "FAILED: check SW6, the J-Link cable and that the board is stopped in U-Boot, then run again" -ForegroundColor Red; exit 1
}
