cmake_minimum_required(VERSION 3.16)
project(cliproxy_key_model_restriction C)

add_library(cliproxy_key_model_restriction SHARED src/plugin.c)
set_target_properties(cliproxy_key_model_restriction PROPERTIES
    OUTPUT_NAME "key-model-restriction-c"
    PREFIX ""
)
