cmake_minimum_required(VERSION 3.7.2)

project(loopy C)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -u __vsyscall_ptr")

# list all source files here
add_executable(loopy EXCLUDE_FROM_ALL src/loopy.c)
# list libraries to link binary against
target_link_libraries(loopy muslc sel4 sosapi)

# warn about everything
add_compile_options(-Wall -Werror -W -Wextra)

add_app(loopy)