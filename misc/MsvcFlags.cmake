# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(DIRECTORY)

# Build multiple source files concurrently.
# https://learn.microsoft.com/en-us/cpp/build/reference/mp-build-with-multiple-processes
add_compile_options(/MP)

# Increase build parallelism for native C++ projects.
# https://devblogs.microsoft.com/cppblog/cpp-build-throughput-investigation-and-tune-up
list(APPEND CMAKE_VS_GLOBALS
    "UseMultiToolTask=true"
    "EnableClServerMode=true"
    "BuildPassReferences=true"
)

# Remove spurious warnings about projects sharing build directories.
# These warnings can be emitted even when no sharing is taking place.
list(APPEND CMAKE_VS_GLOBALS "IgnoreWarnIntDirSharingDetected=true")

add_compile_options(/EHsc /utf-8 /guard:cf /guard:ehcont)
add_link_options(/GUARD:CF /guard:ehcont)

if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    # Signed return addresses help prevent an overwritten return address from
    # redirecting execution. The linker option also protects delay-load thunks.
    # https://learn.microsoft.com/en-us/cpp/build/reference/c-cpp-prop-page#enable-signed-returns
    # https://github.com/microsoft/STL/blob/main/stl/CMakeLists.txt
    add_compile_options(/guard:signret)
    add_link_options(/guard:delayloadsignret)
elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    add_link_options(/CETCOMPAT)
endif()

# Package global data separately so the linker can discard unused data and
# merge identical constants. Retain errors for conflicting definitions.
# https://learn.microsoft.com/en-us/cpp/build/reference/gw-optimize-global-data
# https://learn.microsoft.com/en-us/cpp/build/reference/zc-check-gwodr
add_compile_options($<$<CONFIG:Release>:/Gw> /Zc:checkGwOdr)

# These settings retain `/sdl`'s diagnostics and strict stack-cookie checks.
# In the MSVC ARM64 build, `/sdl` additionally cleared entire `TranscodedText`
# objects, including unused character storage. That observed clearing is not
# explained by its documented pointer-member initialization.
# https://learn.microsoft.com/en-us/cpp/build/reference/sdl-enable-additional-security-checks
add_compile_options(
    /sdl- /GS
    "/FI${CMAKE_CURRENT_LIST_DIR}/../src/modules/include/devicefs/msvc_checks.h"

    # C4146: "unary minus operator applied to unsigned type, result still unsigned"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-2-c4146
    /we4146

    # C4308: "negative integral constant converted to unsigned type"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-2-c4308
    /we4308

    # C4532: "'continue': jump out of __finally/finally block has undefined
    # behavior during termination handling"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-c4532
    /we4532

    # C4533: "initialization of 'variable' is skipped by 'goto label'"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-c4533
    /we4533

    # C4700: "uninitialized local variable 'name' used"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-and-level-4-c4700
    /we4700

    # C4703: "potentially uninitialized local pointer variable 'identifier' used"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4703
    /we4703

    # C4789: "buffer 'identifier' of size N bytes will be overrun;
    # M bytes will be written starting at offset L"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-1-c4789
    /we4789

    # C4995: "'function': name was marked as #pragma deprecated"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-3-c4995
    /we4995

    # C4996: "'deprecated-declaration': deprecation-message"
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-3-c4996
    /we4996
)

# C5260 warns that importing a header unit changes a constant's linkage.
# WIL's `__buffer_size` supplies a buffer size; its object identity is irrelevant
# to that use, so the linkage change does not require modifying WIL.
# https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
add_compile_options(/wd5260)

# Apply additional flags for conformance with standard C++.
add_compile_options(
    /volatile:iso
    /Zc:__cplusplus
    /Zc:enumEncoding
    /Zc:enumTypes
    /Zc:preprocessor
    /Zc:templateScope
    /Zc:throwingNew
    /Zc:u8EscapeEncoding
)

# Reject unrecognized compiler options.
# https://learn.microsoft.com/en-us/cpp/build/reference/options-strict
add_compile_options(/options:strict)

# Treat linker warnings as errors.
# https://learn.microsoft.com/en-us/cpp/build/reference/wx-treat-linker-warnings-as-errors
add_link_options(/WX)

# Enable certain potentially-useful warnings that might be disabled by default.
add_compile_options(
    # C4619: "An attempt was made to disable a warning that does not exist."
    /we4619

    # C5038: "The compiler warns when the initialization order isn't the same
    # as the declaration order of data members or base classes."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c5038
    /we5038

    # C5263: "calling 'std::move' on a temporary object prevents copy elision"
    /we5263

    # C4062: "An enum value is not handled by the switch."
    # A default branch counts as handling the remaining values.
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-4-c4062
    /we4062

    # C4426: "Including a header changed the compiler's optimization settings."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4400-through-c4599
    /we4426

    # C4557: "'__assume' contains an expression with side effects."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warning-level-3-c4557
    /we4557

    # C4643: "A declaration in namespace std violates the standard's
    # restrictions on forward declarations."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4600-through-c4799
    /we4643

    # C4749: "'offsetof' is being used with a non-standard-layout type."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4600-through-c4799
    /we4749

    # C4767: "The linker will truncate this section name because it
    # exceeds eight characters."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4600-through-c4799
    /we4767

    # C4842: "'offsetof' on this multiply inherited type can produce
    # different results with different compiler releases."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4800-through-c4999
    /we4842

    # C4855: "Implicitly capturing 'this' with '[=]' is deprecated."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c4800-through-c4999
    /we4855

    # C5032: "A warning-state push has no corresponding pop."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5000-through-c5199
    /we5032

    # C5215: "Top-level volatile qualification on a function parameter
    # is deprecated."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5215

    # C5216: "Top-level volatile qualification on a function's return
    # type is deprecated."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5216

    # C5240: "An attribute is ignored because of where it appears."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c5240
    /we5240

    # C5243: "A pointer-to-member involving an incomplete class can
    # have inconsistent representations across translation units."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c5243
    /we5243

    # C5247: "Manually creating a section reserved for C++ initialization
    # can interfere with compiler-generated initialization."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c5247
    /we5247

    # C5248: "A variable manually placed in a reserved initialization
    # section may be removed or initialized in an unspecified order."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/c5248
    /we5248

    # C5249: "An enum bit field is too narrow to represent some of
    # the enum's named values."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5249

    # C5250: "An intrinsic function has not been declared."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5250

    # C5251: "Including a header changed a pragma setting without
    # restoring its previous value."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5251

    # C5259: "An explicit specialization is missing 'template <>'."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5259

    # C5291: "This inheritance layout is affected by a compiler ABI bug
    # involving padding at the end of the base class."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5291

    # C5322: "A variable requiring dynamic initialization has been
    # placed in a read-only custom section."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
    /we5322

    # C5031: "A warning-state pop matches a push from a different file."
    # https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5000-through-c5199
    /we5031
)
