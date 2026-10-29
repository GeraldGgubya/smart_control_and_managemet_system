# Picks up every file in src/ automatically. If a newly added .c file is not
# built, run "PlatformIO: Clean" (or touch this file) so CMake re-scans.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources})
