add_executable(test_gte test_gte.c)

target_link_libraries(test_gte PRIVATE
    q2psx_common
    q2psx_psx
)

add_test(NAME gte_conformance COMMAND test_gte)

add_executable(test_sim test_sim.c)

target_link_libraries(test_sim PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME sim_behaviour COMMAND test_sim)

add_executable(test_inventory test_inventory.c)

target_link_libraries(test_inventory PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME inventory COMMAND test_inventory)

add_executable(test_monster test_monster.c)

target_link_libraries(test_monster PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME monster COMMAND test_monster)

add_executable(test_combat test_combat.c)

target_link_libraries(test_combat PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME combat COMMAND test_combat)

add_executable(test_save test_save.c)

target_link_libraries(test_save PRIVATE
    q2psx_common
    q2psx_game
)

add_test(NAME save COMMAND test_save)
