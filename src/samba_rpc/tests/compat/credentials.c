/*
 * SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Samba's public credentials.h contains forward declarations of the unscoped
 * enums smb_signing_setting and smb_encryption_setting without fixed
 * underlying types. Those declarations are valid C but ill-formed C++, so the
 * generated-client test cannot include the header directly. This C adapter
 * keeps that third-party language defect out of the C++ test and exposes only
 * its one credential-construction operation.
 */

#include <credentials.h>

struct cli_credentials *devicefs_test_credentials(TALLOC_CTX *memory,
    struct loadparm_context *configuration, const char *username,
    const char *password)
{
    struct cli_credentials *credentials = cli_credentials_init(memory);
    if (credentials == NULL) {
        return NULL;
    }
    if (!cli_credentials_set_conf(credentials, configuration) ||
        !cli_credentials_set_domain(
            credentials, "DEVICEFSTEST", CRED_SPECIFIED) ||
        !cli_credentials_set_username(
            credentials, username, CRED_SPECIFIED) ||
        !cli_credentials_set_password(
            credentials, password, CRED_SPECIFIED) ||
        !cli_credentials_set_kerberos_state(credentials,
            CRED_USE_KERBEROS_DISABLED, CRED_SPECIFIED)) {
        talloc_free(credentials);
        return NULL;
    }
    return credentials;
}
