cmake_minimum_required(VERSION 3.19)
project(glcomp)

set(CMAKE_CXX_STANDARD 14)

add_executable(glcomp
        main.cpp)

target_link_libraries(glcomp X11 Xcomposite Xfixes Xdamage Xrender Xext)