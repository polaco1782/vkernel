set(LIB progs)

add_library(${LIB} STATIC
    src/pr_cmds.c
    src/pr_edict.c
    src/pr_exec.c
)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common mathlib)
target_link_libraries(${LIB} PRIVATE
    cmd
    console
    crc
    host
    server
    sys
)
