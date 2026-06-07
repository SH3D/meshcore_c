# Compile the portable C core straight from the repo's src/ -- no copy, no
# duplication. The path is relative to this component directory:
#   examples-esp-idf/tty_bridge/main  ->  ../../../src
set(MC_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

idf_component_register(
    SRCS        "main.c" "${MC_CORE_DIR}/meshcore_companion.c"
    INCLUDE_DIRS "." "${MC_CORE_DIR}")
