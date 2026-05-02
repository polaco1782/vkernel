set(LIB renderer)

add_library(${LIB} STATIC
    src/adivtab.h
    src/anorms.h
    src/d_edge.c
    src/d_fill.c
    src/d_init.c
    src/d_modech.c
    src/d_part.c
    src/d_polyse.c
    src/d_scan.c
    src/d_sky.c
    src/d_sprite.c
    src/d_surf.c
    src/d_vars.c
    src/d_zpoint.c
    src/draw.c
    src/nonintel.c
    src/r_aclip.c
    src/r_alias.c
    src/r_bsp.c
    src/r_draw.c
    src/r_edge.c
    src/r_efrag.c
    src/r_light.c
    src/r_main.c
    src/r_misc.c
    src/r_part.c
    src/r_sky.c
    src/r_sprite.c
    src/r_surf.c
    src/r_vars.c
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC
    client
    common
    console
    mathlib
    model
    progs
    video
    wad
)
target_link_libraries(${LIB} PRIVATE
    camera
    cmd
    memory
    server
    screen
    sound
    status_bar
    sys
)
