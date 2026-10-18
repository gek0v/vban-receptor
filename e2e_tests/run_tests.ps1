<#
.SYNOPSIS
    Automated PowerShell Test Runner for VBAN Receptor E2E Test Suite.
.DESCRIPTION
    Runs python e2e_tests/run_tests.py and propagates exit codes.
.PARAMETER Tier
    Optional tier number to run (1, 2, 3, or 4).
.PARAMETER VerboseOutput
    Switch to enable verbose output.
#>
param (
    [ValidateSet(1, 2, 3, 4)]
    [int]$Tier,

    [switch]$VerboseOutput
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir

$argsList = @("$scriptDir\run_tests.py")
if ($Tier) {
    $argsList += "--tier"
    $argsList += "$Tier"
}
if ($VerboseOutput) {
    $argsList += "-v"
}

Write-Host "Invoking VBAN E2E Test Runner..." -ForegroundColor Cyan
& python $argsList
$exitCode = $LASTEXITCODE

if ($exitCode -eq 0) {
    Write-Host "`nAll E2E tests completed successfully!" -ForegroundColor Green
} else {
    Write-Host "`nE2E tests failed with exit code $exitCode" -ForegroundColor Red
}

exit $exitCode
