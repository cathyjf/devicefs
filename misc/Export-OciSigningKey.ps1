# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

#requires -Version 7.4

<#
.SYNOPSIS
Print the DeviceFs signing subkey's public coordinates as a C++ initializer.

.DESCRIPTION
The selected signing subkey must be an ECDSA key on the NIST P-521 curve
(`nistp521` in GnuPG). Other key types and curves are not supported.

The supervisor uses Windows Cryptography Next Generation (CNG) to verify image
signatures. CNG needs the signing key's two elliptic-curve coordinates, X and Y,
as bytes. GnuPG exports that same public key inside an OpenPGP document, together
with the primary key, user identity, and signatures that bind them together.
This script extracts the coordinates from the embedded export and prints the
initializer for `coordinates` in src/modules/supervisor/oci_verification.ixx.
It requires gpg on PATH and writes only the initializer to standard output.

The embedded export was obtained on the machine holding the DeviceFs key with:
    gpg --armor --export EDC7363F595C58D2F07930FEB69A7D95683C6E2A! > public-key.asc
`--armor` produces the printable PUBLIC KEY BLOCK stored below. The export
contains public information only; no private key or passphrase is needed to
run this script. To update the embedded key, replace that entire block with a
new export and set $subkeyFingerprint to the intended signing subkey's full
fingerprint. These commands help inspect the export manually:
    gpg --show-keys --with-subkey-fingerprint public-key.asc
    gpg --list-packets --verbose public-key.asc

The first command displays the primary key and subkey fingerprints. The second
shows the individual OpenPGP packets. In this export, the primary key is RSA;
the signing subkey is the `public sub key packet` with key ID B69A7D95683C6E2A
and algorithm 19 (ECDSA). Its `pkey[0]` identifies the `nistp521` curve, and its
`pkey[1]` is the hexadecimal public point. The script selects the subkey by its
full fingerprint and uses GPG's machine-readable output to obtain that point.

The point contains a byte 04 followed by X and then Y. The 04 means that both
coordinates are present (an uncompressed point). Each P-521 coordinate takes
66 bytes because 521 bits round up to 66 whole bytes. Removing just the 04
leaves the 132 coordinate bytes needed by CNG. The leading 00 in each of this
key's coordinates belongs to its fixed width and must remain. The byte order
also stays unchanged: big-endian means the most significant byte comes first.
The ECDSA key fields and point representation are specified in:
    https://www.rfc-editor.org/rfc/rfc9580.html#section-5.5.5.4
    https://www.rfc-editor.org/rfc/rfc9580.html#section-11.2.1

The C++ code places a BCRYPT_ECCKEY_BLOB header immediately before these bytes.
That header identifies ECDSA P-521 and gives the size of ONE coordinate in
`cbKey`, so CNG can read X followed by Y. OpenPGP's surrounding packets, curve
identifier, and 04 prefix are not part of this Windows representation:
    https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/ns-bcrypt-bcrypt_ecckey_blob

.EXAMPLE
pwsh -NoProfile -File .\misc\Export-OciSigningKey.ps1

When run, this program prints the initializer used in the C++ file.
#>
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$subkeyFingerprint = 'EDC7363F595C58D2F07930FEB69A7D95683C6E2A'
$publicKey = @'
-----BEGIN PGP PUBLIC KEY BLOCK-----

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
=/ZRL
-----END PGP PUBLIC KEY BLOCK-----
'@

# GPG reads the embedded export from standard input with `--show-keys`, so it
# does not need to import anything. `--no-options` skips the user's options file
# and prevents automatic home-directory creation. `--no-keyring` disables keyring
# access, and `--trust-model always` avoids trust-database lookups while listing
# the key. `--no-autostart` prevents GPG from launching gpg-agent or dirmngr.
# The explicit home pathname overrides both GNUPGHOME and the Windows registry
# setting. It names an unused temporary location but never creates it, so this
# operation leaves neither profile files nor a temporary GPG directory behind.
# https://www.gnupg.org/documentation/manuals/gnupg/GPG-Configuration-Options.html
$gpgHome = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
$records = $publicKey | gpg --no-options --homedir $gpgHome `
    --no-keyring --trust-model always --no-autostart --batch `
    --with-colons --with-key-data --show-keys
if ($LASTEXITCODE -ne 0) {
    throw "GPG could not read the embedded public-key export (exit status $LASTEXITCODE)."
}

# `--with-colons` produces one record per line, with fields separated by colons;
# `--with-key-data` adds the public-key parameters. A `pub` record begins a
# primary key and a `sub` record begins a subkey. The following `fpr` (fingerprint)
# and `pkd` (public-key data) records belong to that key until the next pub/sub.
# This association lets us skip the RSA primary key and select the intended
# signing subkey even if the export contains other subkeys.
#
# In GPG's documentation, fields are numbered from 1; PowerShell arrays start
# at 0. Algorithm and curve are fields 4 and 17 of pub/sub, hence [3] and [16].
# The full fingerprint is field 10 of fpr, hence [9]. In pkd, field 2 identifies
# the parameter and field 4 contains its hexadecimal value, hence [1] and [3].
# The colon-record format is documented here:
# https://github.com/gpg/gnupg/blob/gnupg-2.5.21/doc/DETAILS
$isSubkey = $false
$selected = $false
$curve = ''
$algorithm = ''
foreach ($record in $records) {
    $fields = $record.Split(':')
    switch ($fields[0]) {
        { $_ -in 'pub', 'sub' } {
            $isSubkey = $_ -eq 'sub'
            $selected = $false
            $algorithm = $fields[3]
            $curve = $fields[16]
        }
        'fpr' {
            $selected = $isSubkey -and ($fields[9] -eq $subkeyFingerprint)
        }
        'pkd' {
            if (-not $selected -or $fields[1] -ne '1') {
                continue
            }
            if ($algorithm -ne '19' -or $curve -ne 'nistp521') {
                throw "Subkey $subkeyFingerprint is not an ECDSA P-521 key."
            }
            # ECDSA parameter 1 is the point described above. Checking for
            # 1 + 66 + 66 = 133 bytes and the uncompressed-point prefix ensures
            # that splitting the remaining bytes in half yields X and Y.
            $point = [Convert]::FromHexString($fields[3])
            if ($point.Length -ne 133 -or $point[0] -ne 4) {
                throw "Subkey $subkeyFingerprint has an unexpected P-521 point encoding."
            }
            $coordinateSize = ($point.Length - 1) / 2
            '{'
            # The output skips the prefix byte but preserves every coordinate
            # byte. Eleven bytes per line puts each 66-byte coordinate on six
            # lines, matching the layout of the C++ initializer.
            foreach ($coordinate in 0..1) {
                '    // ' + @('x', 'y')[$coordinate]
                $start = 1 + $coordinate * $coordinateSize
                for ($offset = 0; $offset -lt $coordinateSize; $offset += 11) {
                    $last = [Math]::Min($offset + 10, $coordinateSize - 1)
                    $hex = foreach ($i in $offset..$last) {
                        '0x{0:x2}' -f $point[$start + $i]
                    }
                    '    ' + ($hex -join ', ') + ','
                }
            }
            '}'
            return
        }
    }
}

throw "No public point was found for subkey $subkeyFingerprint in the embedded export."
