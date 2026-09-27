idf_component_register(SRCS "sfp_shell.c" INCLUDE_DIRS "include" REQUIRES sfp)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
