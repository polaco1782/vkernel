set(LIB console)

add_library(${LIB} STATIC
    src/console.c
    src/cvar.c
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common)
target_link_libraries(${LIB} PRIVATE
    client
    cmd
    input
    renderer
    screen
    server
    sound
    sys
)
