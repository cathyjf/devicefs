if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "The GCC toolchain requires GNU GCC, but the discovered compiler "
        "(${CMAKE_CXX_COMPILER}) appears to be ${CMAKE_CXX_COMPILER_ID}.")
endif()
