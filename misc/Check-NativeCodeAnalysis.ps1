# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

[CmdletBinding()]
param(
    [string] $BuildDirectory =
        [IO.Path]::Combine($PSScriptRoot, '..', 'build'),

    [string] $Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$reports = @(
    Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File `
            -Filter '*.nativecodeanalysis.sarif' |
        Where-Object { $_.Directory.Name -eq $Configuration } |
        Sort-Object FullName
)
if ($reports.Count -eq 0) {
    throw "No native code analysis reports were found for configuration " +
        "'$Configuration' under '$BuildDirectory'."
}

$has_findings = $false
foreach ($report in $reports) {
    try {
        $sarif = Get-Content -LiteralPath $report.FullName -Raw |
            ConvertFrom-Json
    } catch {
        throw "Could not read native code analysis report " +
            "'$($report.FullName)': $($_.Exception.Message)"
    }

    $report_has_findings = $false
    foreach ($run in $sarif.runs) {
        foreach ($result in $run.results) {
            if (-not $report_has_findings) {
                Write-Output ("Native code analysis findings in " +
                    "'$($report.FullName)':")
                $report_has_findings = $true
                $has_findings = $true
            }

            $physical_location =
                $result.locations[0].physicalLocation
            $artifact_location = $physical_location.artifactLocation
            if ($null -ne $artifact_location.PSObject.Properties['index']) {
                $artifact_location =
                    $run.artifacts[[int]$artifact_location.index].location
            }
            $source_uri = [Uri]$artifact_location.uri
            $source = if ($source_uri.IsFile) {
                $source_uri.LocalPath
            } else {
                $source_uri.AbsoluteUri
            }
            $region = $physical_location.region
            Write-Output ('{0}({1},{2}): error {3}: {4}' -f
                $source, $region.startLine, $region.startColumn,
                $result.ruleId, $result.message.text)
        }
    }
}

if ($has_findings) {
    Write-Output 'Native code analysis reported defects.'
    exit 1
}

Write-Output ("Native code analysis reports examined: " +
    "$($reports.Count); no findings.")
