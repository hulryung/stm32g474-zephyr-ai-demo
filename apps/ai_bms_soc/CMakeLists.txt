cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_bms_soc LANGUAGES C CXX)

set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

target_sources(app PRIVATE
    src/main.c
    src/cmd_soc.c
    src/soc_classical.c
    src/soc_ml.cpp
    src/soc_calib.cpp
    src/model_mlp.cpp
    src/model_lstm.cpp
    src/model_hybrid.cpp
)
