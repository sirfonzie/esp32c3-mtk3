cmake_minimum_required(VERSION 3.16)

# Shared µT-Kernel component lives at the repo root.
set(EXTRA_COMPONENT_DIRS ../../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mtk3c3_tasks)
