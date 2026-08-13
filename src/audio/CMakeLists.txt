add_library(q2psx_audio STATIC
    vag.c
    vag.h
)

target_include_directories(q2psx_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(q2psx_audio PUBLIC q2psx_common q2psx_disc)
