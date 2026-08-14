add_executable(q2psx-inspect main.c cmd_coll.c cmd_exe.c cmd_hud.c cmd_export.c cmd_menu.c cmd_screen.c cmd_weapons.c)

target_link_libraries(q2psx-inspect PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_formats
    q2psx_build
    q2psx_audio
    q2psx_render
    q2psx_menu
    q2psx_screen
    q2psx_game
)
