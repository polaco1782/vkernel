set(LIB client)

add_library(${LIB} STATIC
    src/cl_demo.c
    src/cl_input.c
    src/cl_main.c
    src/cl_parse.c
    src/cl_tent.c
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC
    common
    console
    mathlib
    net
    renderer
    video
)
target_link_libraries(${LIB} PRIVATE
    camera
    cmd
    host
    input
    model
    screen
    server
    sound
    status_bar
    sys
)
