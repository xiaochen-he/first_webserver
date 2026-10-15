#限制CMake的最低版本
cmake_minimum_requeired(VERSION 3.16)
#声明项目
project(my_webserver LANGUAGES CXX)
#创建可执行目标
add_executable(webserver src/server.cpp)
#多个源码时: webserver src/main.cpp
#               src/server.cpp
#               src/http_parser.cpp
#CMake会自动判断哪些文件发生了变化，只重新编译必要部分，最后再链接。
#设置c++标准和警告：
target_compile_features(webserver PRIVATE cxx_std_17)

target_compile_options(webserver PRIVATE
    -Wall
    -Wextra
    -Wepedantic
)
#编译选项只应用于webserver目标
#cmake -S . -B build
#-S .：源代码和CMakeLists.txt在当前目录；
#-B build：生成的构建文件放进build目录。
#最终编译：cmake --build build
#