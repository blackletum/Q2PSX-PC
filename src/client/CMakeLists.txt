add_executable(q2psx main.c)

target_link_libraries(q2psx PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_build
    q2psx_formats
    q2psx_psx
    q2psx_render
    q2psx_audio
    q2psx_game
    SDL3::SDL3
)
