set(LIB model)

add_library(${LIB} STATIC src/model.c)

target_include_directories(${LIB} PRIVATE ${CMAKE_BINARY_DIR} "../")
target_include_directories(${LIB} PUBLIC "./include")
target_link_libraries(${LIB} PUBLIC common mathlib memory renderer)
target_link_libraries(${LIB} PRIVATE console sys)
