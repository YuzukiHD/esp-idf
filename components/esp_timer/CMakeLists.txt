idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    idf_component_register(INCLUDE_DIRS include)
else()
    if(${target} STREQUAL "sun252i-f101")
        set(timer_priv_reqs)
    else()
        set(timer_priv_reqs esp_hal_timg esp_hal_systimer)
    endif()
    set(srcs "src/esp_timer.c"
             "src/esp_timer_init.c"
             "src/system_time.c"
             "src/esp_timer_impl_common.c")
    if(NOT ${target} STREQUAL "sun252i-f101")
        list(APPEND srcs "src/ets_timer_legacy.c")
    endif()

    if(${target} STREQUAL "sun252i-f101")
        list(APPEND srcs "src/esp_timer_impl_f101.c")
    elseif(CONFIG_ESP_TIMER_IMPL_TG0_LAC)
        list(APPEND srcs "src/esp_timer_impl_lac.c")
    elseif(CONFIG_ESP_TIMER_IMPL_SYSTIMER)
        list(APPEND srcs "src/esp_timer_impl_systimer.c")
    endif()

    if(CONFIG_SOC_SYSTIMER_SUPPORT_ETM)
        list(APPEND srcs "src/esp_timer_etm.c")
    endif()

    idf_component_register(SRCS "${srcs}"
                           INCLUDE_DIRS include
                           PRIV_REQUIRES ${timer_priv_reqs}
                           PRIV_INCLUDE_DIRS private_include)

    # Forces the linker to include esp_timer_init.c
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_timer_init_include_func")
endif()
