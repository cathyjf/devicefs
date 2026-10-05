#!/usr/bin/env -S bash -efux -o pipefail
# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

: "${SAMBA_PIDL_TAG:=samba-4.25.0}"

checkout_path=${1-}
[[ -n $checkout_path ]] || {
    checkout_path=$(mktemp -d)
    trap -- 'rm -rf "$checkout_path"' EXIT
}

git clone \
    --branch "${SAMBA_PIDL_TAG}" \
    --depth=1 \
    --filter=blob:none \
    --single-branch \
    --sparse \
    https://gitlab.com/samba-team/samba.git \
    "$checkout_path"
git -C "$checkout_path" sparse-checkout set pidl
pushd "${checkout_path}/pidl"
perl Makefile.PL
make -j
make install
popd
