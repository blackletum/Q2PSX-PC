add_library(q2psx_disc STATIC
    disc.c
    disc.h
)

target_include_directories(q2psx_disc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_disc PUBLIC q2psx_common)
