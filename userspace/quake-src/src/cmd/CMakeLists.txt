set(LIB cmd)

add_library(${LIB} STATIC src/cmd.c)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(cmd PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common)
target_link_libraries(${LIB} PRIVATE client console host sys memory)
