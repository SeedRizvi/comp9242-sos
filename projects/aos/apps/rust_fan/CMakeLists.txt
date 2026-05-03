cmake_minimum_required(VERSION 3.7.2)

project(rust_fan C)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -u __vsyscall_ptr")

# list all source files here
add_executable(rust_fan EXCLUDE_FROM_ALL src/rust_fan.c)
# list libraries to link binary against
target_link_libraries(rust_fan muslc sel4 sosapi)

# warn about everything
add_compile_options(-Wall -Werror -W -Wextra)

add_app(rust_fan)