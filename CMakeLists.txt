cmake_minimum_required(VERSION 3.30)

project(Dino
        VERSION 0.2
        LANGUAGES CXX
        DESCRIPTION "C/C++ -like, statically typed programming language inspired by C, C++, Java and Rust"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLD CONFIG REQUIRED)
find_package(LLVM CONFIG REQUIRED)
find_package(libxml2 CONFIG REQUIRED)

llvm_map_components_to_libnames(LLVM_LIBS
        Core 
        ExecutionEngine 
        MC 
        Object
        OrcJIT
        RuntimeDyld
        Support
        native
        NVPTX
        AllTargetsCodeGens
        AllTargetsDisassemblers
        AllTargetsDescs
        SelectionDAG
        AsmPrinter
        CodeGen
        InstCombine
        Instrumentation
        ScalarOpts
        TransformUtils
        Vectorize
        ipo
        MCParser
        BinaryFormat
        MCDisassembler
)

add_executable(dinoc)
target_include_directories(dinoc PUBLIC "src" ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_link_libraries(dinoc PRIVATE ${LLVM_LIBS} lldCommon lldELF lldCOFF lldMachO lldMinGW)
if (WIN32)
    target_compile_definitions(dinoc PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()
target_sources(dinoc PRIVATE
        "src/dino/main.cpp"

        "src/dino/cli/parser.cpp"
        "src/dino/cli/compile.cpp"
        "src/dino/cli/link.cpp"
        "src/dino/cli/linker.cpp"

        "src/dino/codegen/backend.cpp"

        "src/dino/frontend/token.cpp"
        "src/dino/frontend/lexer.cpp"
        "src/dino/frontend/target.cpp"
        "src/dino/frontend/parser.cpp"
        "src/dino/frontend/sema.cpp"
        "src/dino/frontend/dump.cpp"
        "src/dino/frontend/driver.cpp"
)

if (${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND NOT MSVC)
        target_compile_options(dinoc PRIVATE 
                -fsanitize=address,undefined 
                -D_DISABLE_STRING_ANNOTATION 
                -D_DISABLE_VECTOR_ANNOTATION
        )
        target_link_options(dinoc PRIVATE -fsanitize=address,undefined)
endif ()
        