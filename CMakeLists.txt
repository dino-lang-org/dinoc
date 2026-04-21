cmake_minimum_required(VERSION 3.30)

project(Dino
        VERSION 1.0
        LANGUAGES CXX
        DESCRIPTION "C/C++ -like, statically typed programming language inspired by C, C++, Java and Rust"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dinoc)
target_include_directories(dinoc PUBLIC "src")
target_sources(dinoc PRIVATE
        "src/dino/main.cpp"
        "src/dino/frontend/token.cpp"
        "src/dino/frontend/lexer.cpp"
        "src/dino/frontend/ast.cpp"
        "src/dino/frontend/parser.cpp"
        "src/dino/frontend/sema.cpp"
        "src/dino/frontend/dump.cpp"
        "src/dino/frontend/driver.cpp"
)
