# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The generated declarations are stored beside the supervisor so a test using
# an explicit build path also uses that build's Windows API declarations.
function Add-DeviceFsTestTypes {
    param(
        [Parameter(Mandatory)] [string] $SourcePath,
        [string] $SupervisorPath = (Get-DefaultTestExecutablePath 'backup-supervisor.exe')
    )

    $generated = Join-Path ([IO.Path]::GetDirectoryName($SupervisorPath)) `
        'Windows.Win32.NativeMethods.g.cs'
    if (-not [IO.File]::Exists($generated)) {
        throw "The generated Windows API declarations for the tests are missing at '$generated'. " +
            'Reconfigure and build with NuGet and dotnet available to generate them.'
    }
    Add-Type -Path $SourcePath, $generated -CompilerOptions '/unsafe'
}
