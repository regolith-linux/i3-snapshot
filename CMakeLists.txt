cmake_minimum_required(VERSION 3.10)
project(i3_snapshot)

set(CMAKE_CXX_STANDARD 17)
add_subdirectory(lib/i3ipc++)

include_directories(${I3IPCpp_INCLUDE_DIRS} lib/base64)
link_directories(${I3IPCpp_LIBRARY_DIRS})

add_executable(i3-snapshot src/main.cpp lib/base64/base64.cpp)

target_link_libraries(i3-snapshot ${I3IPCpp_LIBRARIES})

install(TARGETS i3-snapshot
  RUNTIME DESTINATION bin
)