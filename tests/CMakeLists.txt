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
