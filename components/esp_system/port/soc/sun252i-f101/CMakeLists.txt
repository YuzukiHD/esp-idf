set(srcs "cpu_start.c"
         "start.S"
         "system_f101.c"
         "wdt_f101.c")

add_prefix(srcs "${CMAKE_CURRENT_LIST_DIR}/" ${srcs})

target_sources(${COMPONENT_LIB} PRIVATE ${srcs})
