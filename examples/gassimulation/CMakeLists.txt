cmake_minimum_required(VERSION 3.13)
project(gassimulation LANGUAGES CXX)

find_package(Celerity 0.3.2 REQUIRED)

add_executable(gassimulation gassimulation.cc)
set_property(TARGET gassimulation PROPERTY CXX_STANDARD 17)
add_celerity_to_target(TARGET gassimulation SOURCES gassimulation.cc)
target_compile_options(gassimulation -Wignored-attributes -Wunknown-cuda-version -fPIE)
# -Wignored-attributes -Wunknown-cuda-version
include_directories(include)
