add_executable(q2psx-inspect main.c)

target_link_libraries(q2psx-inspect PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_formats
    q2psx_build
)
