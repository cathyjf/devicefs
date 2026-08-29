find_program(LLVM_C_COMPILER
    NAMES clang-23 clang-22 clang
    HINTS /opt/homebrew/opt/llvm/bin
    REQUIRED
)
get_filename_component(CMAKE_C_COMPILER "${LLVM_C_COMPILER}" REALPATH)
get_filename_component(LLVM_BIN_DIRECTORY "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(LLVM_DIRECTORY "${LLVM_BIN_DIRECTORY}" DIRECTORY)
set(CMAKE_CXX_COMPILER "${LLVM_BIN_DIRECTORY}/clang++")

set(CMAKE_AR "${LLVM_BIN_DIRECTORY}/llvm-ar")
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_RANLIB "${LLVM_BIN_DIRECTORY}/llvm-ranlib")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_NM "${LLVM_BIN_DIRECTORY}/llvm-nm")
set(CMAKE_OBJCOPY "${LLVM_BIN_DIRECTORY}/llvm-objcopy")
set(CMAKE_OBJDUMP "${LLVM_BIN_DIRECTORY}/llvm-objdump")
set(CMAKE_STRIP "${LLVM_BIN_DIRECTORY}/llvm-strip")

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    find_program(CMAKE_LINKER ld64.lld
        HINTS "${LLVM_BIN_DIRECTORY}" /opt/homebrew/opt/lld/bin
        REQUIRED
    )
else()
    find_program(CMAKE_LINKER ld.lld
        HINTS "${LLVM_BIN_DIRECTORY}"
        REQUIRED
    )
endif()

set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=${CMAKE_LINKER}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=${CMAKE_LINKER}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=${CMAKE_LINKER}")

find_file(LIBCXX_MODULES_JSON libc++.modules.json
    PATHS
        "${LLVM_DIRECTORY}/lib/c++"
        "${LLVM_DIRECTORY}/lib"
    NO_DEFAULT_PATH
    REQUIRED
)
set(CMAKE_CXX_STDLIB_MODULES_JSON "${LIBCXX_MODULES_JSON}"
    CACHE FILEPATH "libc++ standard-library module metadata"
)
