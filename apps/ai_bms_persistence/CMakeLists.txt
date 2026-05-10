cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ai_bms_persistence LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/bms_state.c
    src/cmd_persist.c
)
