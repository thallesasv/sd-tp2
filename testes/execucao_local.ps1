param(
    [ValidateSet(2, 4)]
    [int]$Peers = 2,
    [string]$DataFile = (Join-Path $PSScriptRoot 'dados\file_a_10kb.bin'),
    [int]$BlockSize = 1024,
    [int]$BasePort = 5000,
    [string]$PeerExe = (Join-Path $PSScriptRoot '..\p2p_peer'),
    [string]$LogDir = (Join-Path $PSScriptRoot 'logs')
)

$ErrorActionPreference = 'Stop'

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

if (-not (Test-Path $PeerExe)) {
    $fallbackExe = "$PeerExe.exe"
    if (Test-Path $fallbackExe) {
        $PeerExe = $fallbackExe
    } else {
        throw "Executavel nao encontrado em $PeerExe. Compile o projeto primeiro."
    }
}

if (-not (Test-Path $DataFile)) {
    throw "Arquivo de dados nao encontrado em $DataFile. Rode gerar_arquivos_teste.ps1 primeiro."
}

$metaFile = "$DataFile.meta"
$seederPort = $BasePort
$leecherPorts = @()
for ($i = 1; $i -lt $Peers; $i++) {
    $leecherPorts += ($BasePort + $i)
}

Write-Host "Iniciando seeder em porta $seederPort..."
$seederLog = Join-Path $LogDir 'peer_0_seeder.log'
$seederArgs = @('--listen', "127.0.0.1:$seederPort", '--input', $DataFile, '--block-size', $BlockSize, '--meta', $metaFile)
for ($i = 0; $i -lt $leecherPorts.Count; $i++) {
    $seederArgs += @('--peer', "127.0.0.1:$($leecherPorts[$i])")
}
$seeder = Start-Process -FilePath $PeerExe -ArgumentList $seederArgs -PassThru -NoNewWindow -RedirectStandardOutput $seederLog -RedirectStandardError $seederLog

$peers = @($seeder)

for ($i = 0; $i -lt $leecherPorts.Count; $i++) {
    $port = $leecherPorts[$i]
    $outputFile = Join-Path $LogDir ("peer_{0}_download.bin" -f ($i + 1))
    $peerLog = Join-Path $LogDir ("peer_{0}.log" -f ($i + 1))
    $args = @('--listen', "127.0.0.1:$port", '--meta', $metaFile, '--output', $outputFile, '--peer', "127.0.0.1:$seederPort", '--block-size', $BlockSize, '--stop-on-complete')
    Write-Host "Iniciando leecher em porta $port..."
    $proc = Start-Process -FilePath $PeerExe -ArgumentList $args -PassThru -NoNewWindow -RedirectStandardOutput $peerLog -RedirectStandardError $peerLog
    $peers += $proc
}

foreach ($proc in $peers | Select-Object -Skip 1) {
    Wait-Process -Id $proc.Id
}

if (-not $seeder.HasExited) {
    Stop-Process -Id $seeder.Id -Force
}

Write-Host "Logs gravados em $LogDir"
