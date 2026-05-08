cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_sine LANGUAGES C CXX)

# TFLM uses local statics whose thread-safe initialization needs the C++ ABI
# helpers Zephyr does not pull in. Disable thread-safe statics for this app.
set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

target_sources(app PRIVATE
    src/main.c
    src/cmd_ai.c
    src/tflm_sine.cpp
    src/model.cpp
)
