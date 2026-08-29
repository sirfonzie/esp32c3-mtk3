idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES mtkernel freertos_shim espmesh blebridge)
