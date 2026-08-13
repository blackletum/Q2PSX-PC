add_library(q2psx_formats STATIC
    collision.c
    collision.h
    dat.c
    dat.h
    level.c
    level.h
    points.c
    points.h
    scene.c
    scene.h
)

target_include_directories(q2psx_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_formats PUBLIC q2psx_common q2psx_disc)
