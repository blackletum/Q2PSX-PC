add_library(q2psx_formats STATIC
    area.c
    area.h
    collision.c
    collision.h
    dat.c
    dat.h
    entity.c
    entity.h
    events.c
    events.h
    level.c
    level.h
    leveltext.c
    leveltext.h
    model.c
    model.h
    population.c
    population.h
    points.c
    points.h
    trigger.c
    trigger.h
    userfuncs.c
    userfuncs.h
    reloc.c
    reloc.h
    scene.c
    scene.h
    spacelights.c
    spacelights.h
    stx.c
    stx.h
    sortdata.c
    sortdata.h
    surface.c
    surface.h
    vram.c
    vram.h
    worldscale.h
)

target_include_directories(q2psx_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_formats PUBLIC q2psx_common q2psx_disc q2psx_psx)
