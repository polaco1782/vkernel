set(LIB server)

add_library(${LIB} STATIC
    src/sv_main.c
    src/sv_move.c
    src/sv_phys.c
    src/sv_user.c
    src/sv_world.c
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC
    client
    common
    console
    mathlib
    model
    net
    progs
)
target_link_libraries(${LIB} PRIVATE
    camera
    cmd
    host
    input
    sound
    sys
)
