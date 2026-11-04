$ErrorActionPreference = 'Stop'
$settingsPath = Join-Path $PSScriptRoot 'Saved\ModKit\engine.json'
$setupPath = Join-Path $PSScriptRoot 'Saved\ModKit\setup.json'
if (!(Test-Path -LiteralPath $settingsPath) -or !(Test-Path -LiteralPath $setupPath)) {
    throw 'Run Setup.ps1 successfully before opening the kit.'
}
$settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
$editor = Join-Path $settings.path 'Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path $PSScriptRoot 'RetakeModKit.uproject'
if (!(Test-Path -LiteralPath $editor)) { throw 'The configured engine moved. Run Setup.ps1 with -EngineRoot again.' }
# This is the creator's explicit interactive editor launch, not a background build.
& $editor $project
exit $LASTEXITCODE
