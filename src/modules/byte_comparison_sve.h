// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// This header is included by, and should only be included by,
// `byte_comparison.ixx` and `byte_comparison_sve.cpp`. Prior to including this
// header, the `DEVICEFS_INSTANTIATE_SVE_REDUCTION` macro should already be
// defined. The interface file expands the macro into declarations. The
// implementation file expands the macro into definitions.

// This is the list of instantiations of the SVE reduction algorithm that are
// currently needed by production callers. If more instantiations are needed
// later, they can be added here.
DEVICEFS_INSTANTIATE_SVE_REDUCTION(32);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(64);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(128);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(256);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(512);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(1024);

// These three instantiations are used only by the tests.
DEVICEFS_INSTANTIATE_SVE_REDUCTION(48);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(96);
DEVICEFS_INSTANTIATE_SVE_REDUCTION(192);
