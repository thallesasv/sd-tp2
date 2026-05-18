param(
    [string]$OutputDir = (Join-Path $PSScriptRoot 'dados')
)

$ErrorActionPreference = 'Stop'

function New-RandomFile {
    param(
        [string]$Path,
        [int]$SizeBytes
    )

    $buffer = New-Object byte[] $SizeBytes
    [System.Security.Cryptography.RandomNumberGenerator]::Fill($buffer)
    [System.IO.File]::WriteAllBytes($Path, $buffer)
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$files = @(
    @{ Name = 'file_a_10kb.bin'; Size = 10KB },
    @{ Name = 'file_a_20kb.bin'; Size = 20KB },
    @{ Name = 'file_b_1mb.bin'; Size = 1MB },
    @{ Name = 'file_b_5mb.bin'; Size = 5MB },
    @{ Name = 'file_c_10mb.bin'; Size = 10MB },
    @{ Name = 'file_c_20mb.bin'; Size = 20MB }
)

foreach ($file in $files) {
    $path = Join-Path $OutputDir $file.Name
    Write-Host "Gerando $($file.Name) ($($file.Size) bytes)..."
    New-RandomFile -Path $path -SizeBytes $file.Size
    $hash = Get-FileHash -Algorithm SHA256 -Path $path
    [PSCustomObject]@{
        File = $file.Name
        SizeBytes = $file.Size
        Sha256 = $hash.Hash
    }
}
