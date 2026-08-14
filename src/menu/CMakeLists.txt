add_library(q2psx_menu STATIC
    menu.c
    menu.h
    menudraw.c
    menudraw.h
    pages.c
)

target_include_directories(q2psx_menu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_menu PUBLIC q2psx_common q2psx_render)
