add_library(q2psx_build STATIC
    classtable.c
    classtable.h
    exe.c
    exe.h
    fxtables.c
    fxtables.h
    vmtables.c
    vmtables.h
    hudtables.c
    hudtables.h
    mips.c
    mips.h
    icontable.c
    icontable.h
    ident.c
    leveltable.c
    leveltable.h
    ident.h
    itemtable.c
    itemtable.h
    weapontables.c
    weapontables.h
)

target_include_directories(q2psx_build PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_build PUBLIC q2psx_common q2psx_disc q2psx_psx)
