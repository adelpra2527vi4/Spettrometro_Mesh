cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(as7341_mesh_node)
target_sources(app PRIVATE src/main.c)
