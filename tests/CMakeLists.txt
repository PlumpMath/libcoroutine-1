# CMake file for tests directory 

enable_testing()
find_package(GTest QUIET)

file(GLOB srcs
	"*.cpp"
	"*.h"
)

add_executable(tests ${srcs} gtest/gtest-all.cc)

target_link_libraries(tests coroutine)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_LIST_DIR})

add_dependencies(tests coroutine)

add_test(libcoroutine-tests tests COMMAND $<TARGET_FILE:tests>)

