add_executable(q2psx main.c gamepad.c)

target_link_libraries(q2psx PRIVATE
    q2psx_common
    q2psx_disc
    q2psx_build
    q2psx_formats
    q2psx_psx
    q2psx_render
    q2psx_menu
    q2psx_screen
    q2psx_audio
    q2psx_game
    SDL3::SDL3
)

# Imported SDL builds need the same runtime placement as fetched SDL builds.
# Otherwise a successful MSVC link produces an executable that cannot start.
if(WIN32)
    get_target_property(q2psx_sdl_type SDL3::SDL3 TYPE)
    if(q2psx_sdl_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET q2psx POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:q2psx>
            VERBATIM)
    endif()

    # Always refresh the local launch directory, even when q2psx is up to date.
    # Copy only build-owned files; disc images, saves and settings stay in place.
    set(q2psx_stage_files
        $<TARGET_FILE:q2psx>
        "${PROJECT_SOURCE_DIR}/scripts/Play.cmd"
        "${PROJECT_SOURCE_DIR}/LICENSE")
    if(q2psx_sdl_type STREQUAL "SHARED_LIBRARY")
        list(APPEND q2psx_stage_files $<TARGET_FILE:SDL3::SDL3>)
    endif()
    add_custom_target(stage-local ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/.install"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${q2psx_stage_files} "${PROJECT_SOURCE_DIR}/.install"
        DEPENDS q2psx
        VERBATIM)
endif()

# A release archive puts the client and a fetched libSDL3 in the same directory,
# so the loader has to look there. CMake appends this to the rpath it computes
# for the build tree rather than replacing it, so running out of build/bin keeps
# working whether SDL3 came from the system or from FetchContent.
if(APPLE)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "@executable_path")
elseif(UNIX)
    set_target_properties(q2psx PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
