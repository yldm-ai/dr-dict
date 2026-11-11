# Builds DR Dict with uiAccess=true, installs it under Program Files and signs it with a local test
# certificate, so the Ctrl+C+C popup can stay above every window (elevated ones, the Start menu and
# the taskbar included).
#
# Windows honours uiAccess only for an executable that is
#   1. signed by a certificate the machine trusts, and
#   2. installed in a secure location (Program Files, System32).
# An unsigned uiAccess build, or one started from any other folder, refuses to start. That is why
# normal builds keep uiAccess off (CMake option DR_DICT_UIACCESS).
#
# Run from an administrator PowerShell:
#   pwsh -File .\scripts\install-uiaccess.ps1
#
# The certificate is created on the first run (CurrentUser\My) and trusted in LocalMachine\Root and
# TrustedPublisher; later runs reuse it. It is a per-machine test certificate: never distribute
# builds signed with it. Adapted from MSIME-Windows installer/Sign-PackageBinaries-Local.ps1.
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'DR Dict'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$CertificateSubject = 'CN=DR Dict Local Test Code Signing'
$CodeSigningEku = '1.3.6.1.5.5.7.3.3'
$TrustStores = @('Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher')
$projectRoot = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $projectRoot 'build-uiaccess'

$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '需要管理员 PowerShell：信任证书和写入 Program Files 都需要管理员权限。'
}

function Find-SignTool {
    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $candidate = Get-ChildItem -LiteralPath $kitsBin -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) { throw '找不到 signtool.exe；请安装 Windows SDK。' }
    return $candidate
}

function Get-TestCertificate {
    $now = Get-Date
    $existing = @(Get-ChildItem 'Cert:\CurrentUser\My' | Where-Object {
            $_.Subject -eq $CertificateSubject -and $_.HasPrivateKey -and $_.NotBefore -le $now -and $_.NotAfter -gt $now -and
            ($_.EnhancedKeyUsageList.ObjectId -contains $CodeSigningEku)
        } | Sort-Object NotAfter -Descending)
    if ($existing.Count) {
        Write-Host '已有本机测试证书，复用。'
        $certificate = $existing[0]
    }
    else {
        $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $CertificateSubject `
            -FriendlyName 'DR Dict Local Test Code Signing' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
            -KeyUsage DigitalSignature -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(5)
        Write-Host '已创建本机测试证书。'
    }
    $missing = @($TrustStores | Where-Object { -not (Test-Path -LiteralPath (Join-Path $_ $certificate.Thumbprint)) })
    if ($missing.Count) {
        $file = Join-Path ([IO.Path]::GetTempPath()) "drdict-local-test-$($certificate.Thumbprint).cer"
        try {
            Export-Certificate -Cert $certificate -FilePath $file -Type CERT -Force | Out-Null
            foreach ($store in $missing) {
                Import-Certificate -FilePath $file -CertStoreLocation $store | Out-Null
                Write-Host "已信任：$store"
            }
        }
        finally { Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue }
    }
    return $certificate
}

if (-not $SkipBuild) {
    Push-Location $projectRoot
    try {
        pnpm install --frozen-lockfile
        if ($LASTEXITCODE) { throw '前端依赖安装失败' }
    }
    finally { Pop-Location }
    cmake -S $projectRoot -B $buildDir -A x64 -DDR_DICT_UIACCESS=ON
    if ($LASTEXITCODE) { throw 'CMake 配置失败' }
    cmake --build $buildDir --config Release --parallel --target dr-dict
    if ($LASTEXITCODE) { throw '构建失败' }
}
$output = Join-Path $buildDir 'bin\Release'
if (-not (Test-Path (Join-Path $output 'dr-dict.exe'))) { throw "找不到构建产物：$output" }

# A running copy keeps the executable locked.
Get-Process dr-dict -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, 'OrdinalIgnoreCase') } |
    ForEach-Object { $_.CloseMainWindow() | Out-Null; if (-not $_.WaitForExit(5000)) { $_.Kill() } }

New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item (Join-Path $output 'dr-dict.exe') $InstallDir -Force
foreach ($folder in 'web', 'opencc') {
    $target = Join-Path $InstallDir $folder
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Copy-Item (Join-Path $output $folder) $target -Recurse -Force
}

$certificate = Get-TestCertificate
$signTool = Find-SignTool
$exe = Join-Path $InstallDir 'dr-dict.exe'
# A local test certificate gains nothing from a timestamp, and the machine may be offline.
& $signTool sign /sha1 $certificate.Thumbprint /s My /fd sha256 $exe
if ($LASTEXITCODE) { throw "签名失败（signtool 退出码 $LASTEXITCODE）" }
& $signTool verify /pa $exe
if ($LASTEXITCODE) { throw '签名校验失败：证书未被本机信任？' }

$shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'DR Dict.lnk'
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($shortcut)
$link.TargetPath = $exe
$link.WorkingDirectory = $InstallDir
$link.Save()

Write-Host ''
Write-Host "已安装到 $InstallDir，并创建开始菜单快捷方式“DR Dict”。"
Write-Host 'uiAccess 程序只能经由 Shell 启动（开始菜单、资源管理器或 Start-Process），不能用 CreateProcess 直接拉起。'
