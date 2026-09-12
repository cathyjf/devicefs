find_program(GCC_C_COMPILER
    NAMES gcc-16 gcc
    HINTS
        /opt/homebrew/opt/gcc/bin
        /opt/gcc-16/bin
    REQUIRED
)

get_filename_component(CMAKE_C_COMPILER "${GCC_C_COMPILER}" REALPATH)
get_filename_component(GCC_BIN_DIRECTORY "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(GCC_C_COMPILER_FILENAME "${CMAKE_C_COMPILER}" NAME)
if(GCC_C_COMPILER_FILENAME MATCHES "gcc(-.*)$")
    set(GCC_VERSION_SUFFIX "${CMAKE_MATCH_1}")
endif()

set(CMAKE_CXX_COMPILER "${GCC_BIN_DIRECTORY}/g++${GCC_VERSION_SUFFIX}")

set(CMAKE_AR "${GCC_BIN_DIRECTORY}/gcc-ar${GCC_VERSION_SUFFIX}")
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")

set(CMAKE_RANLIB "${GCC_BIN_DIRECTORY}/gcc-ranlib${GCC_VERSION_SUFFIX}")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")

set(CMAKE_NM "${GCC_BIN_DIRECTORY}/gcc-nm${GCC_VERSION_SUFFIX}")

find_program(GNU_OBJCOPY
    NAMES gobjcopy objcopy
    HINTS /opt/homebrew/opt/binutils/bin
    OPTIONAL
)
if(GNU_OBJCOPY)
    get_filename_component(CMAKE_OBJCOPY "${GNU_OBJCOPY}" REALPATH)
    get_filename_component(GNU_BINUTILS_DIRECTORY "${CMAKE_OBJCOPY}" DIRECTORY)

    set(CMAKE_OBJDUMP "${GNU_BINUTILS_DIRECTORY}/objdump")
    set(CMAKE_STRIP "${GNU_BINUTILS_DIRECTORY}/strip")
endif()

list(APPEND CMAKE_PROJECT_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/gcc-verify.cmake")
