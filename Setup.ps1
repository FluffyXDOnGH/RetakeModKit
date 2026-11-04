param([string]$EngineRoot, [switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
$setupArguments = @((Join-Path $PSScriptRoot 'Scripts\ModKit\retake_setup.py'), '--kit', $PSScriptRoot)
if ($EngineRoot) { $setupArguments += @('--engine', $EngineRoot) }
if ($CheckOnly) { $setupArguments += '--check-only' }
if (Get-Command py -ErrorAction SilentlyContinue) {
    & py -3 @setupArguments
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    & python @setupArguments
} else {
    throw 'Install Python 3.11 or newer, then run Setup.ps1 again.'
}
exit $LASTEXITCODE
