add_library(q2psx_build STATIC
    classtable.c
    classtable.h
    exe.c
    exe.h
    hudtables.c
    hudtables.h
    mips.c
    mips.h
    ident.c
    leveltable.c
    leveltable.h
    ident.h
    weapontables.c
    weapontables.h
)

target_include_directories(q2psx_build PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_build PUBLIC q2psx_common q2psx_disc)
