add_library(q2psx_common STATIC
    q2psx.c
    q2psx.h
    fixed.h
    sha256.c
    sha256.h
)

target_include_directories(q2psx_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_common PUBLIC q2psx_flags)
