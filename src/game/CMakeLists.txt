add_library(q2psx_game STATIC
    ai.c
    ai.h
    events_rt.c
    events_rt.h
    hud.c
    hud.h
    aimodule.c
    aimodule.h
    combat.c
    combat.h
    inventory.c
    inventory.h
    monster.c
    monster.h
    modeldraw.c
    modeldraw.h
    mover.c
    mover.h
    pickup.c
    pickup.h
    projectile.c
    projectile.h
    weapon.c
    weapon.h
    spawn.c
    spawn.h
    save.c
    save.h
    sim.c
    sim.h
    simcombat.c
    trace.c
    trace.h
    world.c
    world.h
)

target_include_directories(q2psx_game PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_game PUBLIC
    q2psx_common
    q2psx_build
    q2psx_disc
    q2psx_formats
    q2psx_psx
)
