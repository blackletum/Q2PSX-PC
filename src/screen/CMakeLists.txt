add_library(q2psx_screen STATIC
    screen.c
    screen.h
)

target_include_directories(q2psx_screen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_screen PUBLIC
    q2psx_common
    q2psx_build
    q2psx_psx
    q2psx_render
)
