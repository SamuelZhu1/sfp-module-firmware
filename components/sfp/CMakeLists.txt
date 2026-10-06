idf_component_register(
    SRCS "src/sfp_i2c.c" "src/sfp_decode.c" "src/sfp_core.c" "port/esp32/sfp_port_esp32.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES driver esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
if(CONFIG_SFP_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC SFP_TEST_HOOKS)
endif()
