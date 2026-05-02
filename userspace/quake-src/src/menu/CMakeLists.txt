set(LIB menu)

add_library(${LIB} STATIC src/menu.c)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common)
target_link_libraries(${LIB} PRIVATE
    ${SDL2_LIBRARIES}
    camera
    client
    cmd
    console
    host
    input
    renderer
    screen
    server
    sound
    video
)
