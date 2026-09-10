find_program(GCC_C_COMPILER
    NAMES gcc-16
    HINTS /opt/homebrew/opt/gcc/bin
    REQUIRED
)

get_filename_component(CMAKE_C_COMPILER "${GCC_C_COMPILER}" REALPATH)
get_filename_component(GCC_BIN_DIRECTORY "${CMAKE_C_COMPILER}" DIRECTORY)
set(CMAKE_CXX_COMPILER "${GCC_BIN_DIRECTORY}/g++-16")

set(CMAKE_AR "${GCC_BIN_DIRECTORY}/gcc-ar-16")
set(CMAKE_C_COMPILER_AR "${CMAKE_AR}")
set(CMAKE_CXX_COMPILER_AR "${CMAKE_AR}")

set(CMAKE_RANLIB "${GCC_BIN_DIRECTORY}/gcc-ranlib-16")
set(CMAKE_C_COMPILER_RANLIB "${CMAKE_RANLIB}")
set(CMAKE_CXX_COMPILER_RANLIB "${CMAKE_RANLIB}")

set(CMAKE_NM "${GCC_BIN_DIRECTORY}/gcc-nm-16")

find_program(GNU_OBJCOPY
    NAMES gobjcopy objcopy
    HINTS /opt/homebrew/opt/binutils/bin
    REQUIRED
)

get_filename_component(CMAKE_OBJCOPY "${GNU_OBJCOPY}" REALPATH)
get_filename_component(GNU_BINUTILS_DIRECTORY "${CMAKE_OBJCOPY}" DIRECTORY)

set(CMAKE_OBJDUMP "${GNU_BINUTILS_DIRECTORY}/objdump")
set(CMAKE_STRIP "${GNU_BINUTILS_DIRECTORY}/strip")
