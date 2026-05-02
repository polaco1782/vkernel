set(LIB net)

add_library(${LIB} STATIC
    src/net_dgrm.c
    src/net_dgrm.h
    src/net_drivers.c
    src/net_loop.c
    src/net_loop.h
    src/net_main.c
    src/net_poll.c
    src/net_poll.h
    src/net_socket.c
    src/net_socket.h
    src/net_udp.c
    src/net_udp.h
    src/net_vcr.c
    src/net_vcr.h
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC
    $<IF:$<TARGET_EXISTS:SDL2_net::SDL2_net>,SDL2_net::SDL2_net,SDL2_net::SDL2_net-static>
    common
    console
)
target_link_libraries(${LIB} PRIVATE
    client
    cmd
    host
    input
    memory
    screen
    server
    sys
)
