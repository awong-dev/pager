cmake_minimum_required(VERSION 3.16)
# Reuse the repo's patched walter-modem component; do not copy it.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../firmware/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(keepalive_probe)
