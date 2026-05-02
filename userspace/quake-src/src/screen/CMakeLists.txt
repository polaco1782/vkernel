set(LIB screen)

add_library(${LIB} STATIC src/screen.c)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common console)
target_link_libraries(${LIB} PRIVATE
    camera
    client
    cmd
    renderer
    host
    input
    menu
    status_bar
    sound
    sys
    video
    wad
)
