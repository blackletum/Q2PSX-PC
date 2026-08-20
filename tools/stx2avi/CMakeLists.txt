add_executable(stx2avi main.c)

target_link_libraries(stx2avi PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_formats
    q2psx_audio
    q2psx_game
)
