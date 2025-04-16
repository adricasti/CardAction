cmake_minimum_required(VERSION 3.10)
project(CardAction)

set(CMAKE_CXX_STANDARD 14)

# Set Windows subsystem
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS")

# Add source files
add_executable(CardAction WIN32
    CardAction.cpp
    CardAction.rc
)

# Add libraries
target_link_libraries(CardAction
    winscard.lib
    comctl32.lib
)
