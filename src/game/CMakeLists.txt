add_library(q2psx_game STATIC
    events_rt.c
    events_rt.h
    sim.c
    sim.h
    world.c
    world.h
)

target_include_directories(q2psx_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_game PUBLIC
    q2psx_common
    q2psx_disc
    q2psx_formats
    q2psx_psx
)
