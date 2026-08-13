add_library(q2psx_psx STATIC
    gte.c
    gte.h
    gpu.c
    gpu.h
)

target_include_directories(q2psx_psx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_psx PUBLIC q2psx_common)
