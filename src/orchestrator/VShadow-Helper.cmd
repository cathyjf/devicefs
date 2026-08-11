@rem SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick (cathy@cathyjf.com)
@rem SPDX-License-Identifier: GPL-3.0-or-later
@echo off
"%VSHADOW_PWSH_PATH%" -NoLogo -NoProfile -NonInteractive -File "%VSHADOW_COMPLETION_SCRIPT%"
exit /b %ERRORLEVEL%
