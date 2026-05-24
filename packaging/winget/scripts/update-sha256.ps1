param(
    [Parameter(Mandatory = $true)]
    [string] $Version,

    [string] $ExePath,
    [string] $Url
)

$ErrorActionPreference = "Stop"
$manifestDir = Join-Path $PSScriptRoot "..\manifests\n\nobu121\win3drag\$Version"
$installerYaml = Join-Path $manifestDir "nobu121.win3drag.installer.yaml"

if (-not (Test-Path $installerYaml)) {
    throw "Manifest not found: $installerYaml"
}

if ($ExePath) {
    if (-not (Test-Path $ExePath)) { throw "File not found: $ExePath" }
    $hash = (Get-FileHash -Path $ExePath -Algorithm SHA256).Hash
}
elseif ($Url) {
    $tmp = Join-Path $env:TEMP "3drag-winget-hash.exe"
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing
    $hash = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash
    Remove-Item $tmp -Force
}
else {
    throw "Specify -ExePath or -Url"
}

$content = Get-Content $installerYaml -Raw
$content = $content -replace "InstallerSha256:\s*\S+", "InstallerSha256: $hash"
Set-Content -Path $installerYaml -Value $content.TrimEnd() -NoNewline
Add-Content -Path $installerYaml -Value ""

Write-Host "Updated $installerYaml"
Write-Host "InstallerSha256: $hash"
