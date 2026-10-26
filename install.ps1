<#
.SYNOPSIS
  One-click installer for Inkterface Gabe Edition (Windows).
.DESCRIPTION
  Flashes the firmware onto the Feather over USB and installs the host app
  (AppImage + user service) on the Steam Machine over SSH.
.EXAMPLE
  .\install.ps1 -SteamHost steammachine -SteamUser deck
  .\install.ps1 -SkipApp            # firmware only
  .\install.ps1 -DryRun -SteamHost 192.168.2.127
#>
[CmdletBinding()]
param(
    [ValidateSet('', 'v1', 'v2')][string]$Board = '',
    [string]$Port = '',
    [string]$Version = 'latest',
    [string]$LocalDir = '',
    [string]$SteamHost = '',
    [string]$SteamUser = 'deck',
    [string]$SshKey = '',
    [switch]$SkipFirmware,
    [switch]$SkipApp,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo = 'JoppeJoppinger/Inkterface_GabeEdition'
$EsptoolVersion = 'v5.6.0'
$EsptoolSha256 = 'd55a96fbeb97fd4cb05bd93f201e5c8f6518b7bfcde2fc6966fd0190fd891629'
$Cache = Join-Path $env:LOCALAPPDATA 'inkterface-installer'
New-Item -ItemType Directory -Force $Cache | Out-Null

if ($SkipFirmware -and $SkipApp) { throw 'Nothing to do.' }
$BaseUrl = if ($Version -eq 'latest') { "https://github.com/$Repo/releases/latest/download" }
           else { "https://github.com/$Repo/releases/download/$Version" }

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

function Invoke-Step([string]$Exe, [string[]]$Arguments) {
    if ($DryRun) { Write-Host "[dry-run] $Exe $($Arguments -join ' ')"; return }
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

function Get-ReleaseFile([string]$Name) {
    if ($LocalDir) {
        $p = Join-Path $LocalDir $Name
        if (-not (Test-Path $p)) { throw "$p not found" }
        return $p
    }
    $dest = Join-Path $Cache $Name
    if ($DryRun) { Write-Host "[dry-run] download $BaseUrl/$Name"; return $dest }
    try { Invoke-WebRequest "$BaseUrl/$Name" -OutFile $dest -UseBasicParsing }
    catch { throw "Download of $Name failed. No release published yet? Try -LocalDir. ($_)" }
    return $dest
}

function Confirm-ReleaseFile([string]$Path, [string]$Name) {
    if ($LocalDir -or $DryRun) { return }
    $sums = Join-Path $Cache 'SHA256SUMS'
    Invoke-WebRequest "$BaseUrl/SHA256SUMS" -OutFile $sums -UseBasicParsing
    $line = Get-Content $sums | Where-Object { $_ -match "\s\*?$([regex]::Escape($Name))$" } | Select-Object -First 1
    if (-not $line) { throw "No checksum for $Name" }
    $expected = ($line -split '\s+')[0].ToLower()
    $actual = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
    if ($expected -ne $actual) { throw "Checksum mismatch for $Name" }
}

function Install-Esptool {
    $dir = Join-Path $Cache 'esptool-windows-amd64'
    $exe = Join-Path $dir 'esptool.exe'
    if (Test-Path $exe) { return $exe }
    Write-Step "Downloading esptool $EsptoolVersion"
    if ($DryRun) { Write-Host '[dry-run] download and extract esptool'; return $exe }
    $zip = Join-Path $Cache 'esptool.zip'
    Invoke-WebRequest "https://github.com/espressif/esptool/releases/download/$EsptoolVersion/esptool-$EsptoolVersion-windows-amd64.zip" -OutFile $zip -UseBasicParsing
    if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLower() -ne $EsptoolSha256) { throw 'esptool checksum mismatch' }
    Expand-Archive $zip -DestinationPath $Cache -Force
    return $exe
}

function Get-CandidatePorts {
    # Only probe USB serial chips used on ESP32 boards (Espressif, CH340/CH9102, CP210x, FTDI)
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\((COM\d+)\)' -and $_.PNPDeviceID -match 'VID_(303A|1A86|10C4|0403)' } |
        ForEach-Object { [void]($_.Name -match '\((COM\d+)\)'); $Matches[1] }
}

function Install-Firmware {
    Write-Step 'Flashing firmware'
    $esptool = Install-Esptool
    $portArgs = @()
    if ($Port) { $portArgs = @('--port', $Port) }
    $script:Board = $Board

    if (-not $script:Board) {
        if ($DryRun) {
            $script:Board = 'v2'
            Write-Host '[dry-run] board auto-detect via esptool chip-id (assuming v2)'
        } else {
            Write-Host 'Detecting board (connect the Feather via USB)...'
            $candidates = if ($Port) { @($Port) } else { @(Get-CandidatePorts) }
            $info = ''
            $found = $null
            $prev = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            foreach ($candidate in $candidates) {
                Write-Host "Trying $candidate ..."
                $out = (& $esptool --port $candidate chip-id 2>&1 | Out-String)
                if ($LASTEXITCODE -eq 0) { $info = $out; $found = $candidate; break }
            }
            $ErrorActionPreference = $prev
            if (-not $found) {
                throw 'No Feather found. Use a data USB cable; hold BOOT and press RESET to enter the bootloader. If no COM port appears, install the CH9102/CP210x USB driver. You can also pass -Port COMx.'
            }
            $portArgs = @('--port', $found)
            if ($info -match 'ESP32-S3') { $script:Board = 'v1' }
            elseif ($info -match 'ESP32') { $script:Board = 'v2' }
            else { throw 'Unrecognised chip; use -Board v1|v2' }
            Write-Host "Detected board: $($script:Board) on $found"
        }
    }

    $name = "inkterface-firmware-feather$($script:Board).bin"
    $file = Get-ReleaseFile $name
    Confirm-ReleaseFile $file $name
    if ($script:Board -eq 'v1') { Write-Host 'Note: the V1 firmware has not been tested on hardware.' }
    Invoke-Step $esptool ($portArgs + @('write-flash', '0x0', $file))
    Write-Host "Firmware flashed. The display shows 'Waiting on connection' until the host app connects."
}

function Install-App {
    Write-Step 'Installing host app on the Steam Machine'
    foreach ($tool in 'ssh', 'scp') {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "$tool not found. Enable 'OpenSSH Client' in Windows optional features."
        }
    }
    if (-not $SteamHost) {
        $SteamHost = Read-Host 'Steam Machine host name or IP'
        if (-not $SteamHost) { throw 'Steam Machine host is required (-SteamHost).' }
    }
    $file = Get-ReleaseFile 'Inkterface.AppImage'
    Confirm-ReleaseFile $file 'Inkterface.AppImage'

    $target = "$SteamUser@$SteamHost"
    $sshOpts = @('-o', 'StrictHostKeyChecking=accept-new')
    if ($SshKey) { $sshOpts += @('-i', $SshKey) }
    Write-Host "Connecting to $target (SSH must be enabled on the Steam Machine; enter the password if asked)."

    Invoke-Step 'ssh' ($sshOpts + @($target, 'mkdir -p ~/Applications ~/.config/systemd/user && (systemctl --user stop inkterface.service 2>/dev/null || true)'))
    Invoke-Step 'scp' ($sshOpts + @($file, "${target}:Applications/Inkterface.AppImage"))

    $unit = "[Unit]`nDescription=inkterface`nAfter=network.target`n`n[Service]`nType=simple`nExecStart=%h/Applications/Inkterface.AppImage --headless`nRestart=on-failure`n`n[Install]`nWantedBy=default.target`n"
    $unitFile = Join-Path $Cache 'inkterface.service'
    [IO.File]::WriteAllText($unitFile, $unit, (New-Object Text.UTF8Encoding($false)))
    Invoke-Step 'scp' ($sshOpts + @($unitFile, "${target}:.config/systemd/user/inkterface.service"))
    Invoke-Step 'ssh' ($sshOpts + @($target, 'chmod +x ~/Applications/Inkterface.AppImage && systemctl --user daemon-reload && systemctl --user enable inkterface.service && systemctl --user restart inkterface.service'))

    Write-Host 'Service installed and started.'
    Write-Host 'On the Steam Machine: enable Bluetooth (do NOT pair the Inkterface) and run'
    Write-Host '~/Applications/Inkterface.AppImage once in Desktop Mode to select your panel (INKTF-xxxxxx).'
    Write-Host 'Afterwards you may disable SSH again.'
}

if (-not $SkipFirmware) { Install-Firmware }
if (-not $SkipApp) { Install-App }
Write-Step 'Done'
