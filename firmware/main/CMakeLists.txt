idf_component_register(SRCS "app_main.c" "app_ui.c" "ui_memory.c" "app_state.c" "sensors.c" "network_time.c" "wifi_store.c" "wifi_ui.c" "weather_data.c" "weather_schedule.c" "weather_service.c" "location_data.c" "location_store.c" "location_service.c" "location_ui.c"
    "location_catalog.c" "location_index/index.c" "../../assets/application/location_normalize.c"
    "../../assets/application/pinyin_dict.c"
    "../../assets/application/font_28.c" "../../assets/application/font_44.c"
    "../../assets/application/font_240.c" "../../assets/application/home_assets.c" "../../assets/application/font_weather_24.c" "../../assets/application/images.c"
    "../../assets/application/weather_forecast_icons.c"
    INCLUDE_DIRS "." REQUIRES esp_driver_uart esp_driver_i2c esp_timer esp_hw_support esp_app_format
    PRIV_REQUIRES esp_wifi esp_netif esp_event lwip esp_http_client esp-tls mbedtls nvs_flash esp_driver_i2s esp_driver_gpio espressif__zlib
    EMBED_FILES "../../assets/application/gear_icon.bin" "../../assets/application/house_icon.bin"
    "../../assets/application/softTemp_icon.bin" "../../assets/application/softDrop_icon.bin"
    "../../assets/application/weather_cloudunknown.bin" "../../assets/application/weather_sunny.bin"
    "../../assets/application/weather_cloudy.bin" "../../assets/application/weather_overcast.bin"
    "../../assets/application/weather_fog.bin" "../../assets/application/weather_rain.bin"
    "../../assets/application/weather_snow.bin" "../../assets/application/weather_thunder.bin"
    "../../assets/application/locations.lco" "../../assets/application/location_metadata.bin")
set_source_files_properties(app_main.c app_ui.c location_catalog.c location_data.c location_store.c location_service.c location_ui.c weather_data.c weather_service.c location_index/index.c PROPERTIES COMPILE_OPTIONS "-fstack-usage")
# LVGL refers to the custom core after the application archive is scanned.
# Keep its single implementation object in the final link.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")

# Contact data stays outside tracked sources, including generated C sources.
idf_build_get_property(weather_python PYTHON)
set(weather_contact_script "${CMAKE_CURRENT_LIST_DIR}/../../tools/prepare-weather-contact.py")
set(weather_contact_config "${CMAKE_CURRENT_LIST_DIR}/../../.local/weather-provider-contact.json")
set(weather_contact_blob "${CMAKE_CURRENT_BINARY_DIR}/weather_met_contact.bin")
execute_process(COMMAND "${weather_python}" "${weather_contact_script}" --output "${weather_contact_blob}"
    RESULT_VARIABLE weather_contact_result OUTPUT_QUIET ERROR_QUIET)
if(NOT weather_contact_result EQUAL 0)
    message(FATAL_ERROR "Private MET contact configuration preparation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${weather_contact_script}")
if(EXISTS "${weather_contact_config}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${weather_contact_config}")
endif()
target_add_binary_data(${COMPONENT_LIB} "${weather_contact_blob}" BINARY)
# The optional host symbol is weak, but the device configuration must be linked
# even when the application archive is scanned without --whole-archive.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,_binary_weather_met_contact_bin_start")

# S4/S7: reviewed components have real application call sites; one alarm owner.
target_sources(${COMPONENT_LIB} PRIVATE
    ../features/met-weather/met_forecast.c
    display_policy.c background_runtime.c weather_background.c
    ../../assets/weather-background/fields.c ../../assets/application/comfort_images.c
    ../features/environment/environment_runtime.c ../features/environment/environment_app_state.c
    ../features/alarm-management/alarm_config.c ../features/alarm-management/alarm_controller.c
    ../features/alarm-management/alarm_service.c ../features/alarm-management/alarm_runtime.c
    ../features/alarm-management/alarm_ui.c
    ../features/alarm-trigger/alarm_trigger.c ../features/alarm-trigger/alarm_trigger_audio.c
    ../features/alarm-store/alarm_store.c ../features/alarm-store/alarm_store_codec.c ../features/alarm-store/alarm_store_nvs.c
    ../features/alarm-audio/alarm_source.c ../features/alarm-audio/alarm_reference.c
    ../features/alarm-audio/alarm_player.c ../features/alarm-audio/alarm_bsp.c
    ../features/calendar-client/calendar_client.c ../features/calendar-client/calendar_http.c
    ../features/calendar-runtime/calendar_runtime_core.c ../features/calendar-runtime/calendar_runtime_nvs.c ../features/calendar-runtime/calendar_runtime_service.c
    ../core/brightness_policy.c ../core/comfort.c
    ../core/solar_core.c ../core/alarm_core.c)
target_include_directories(${COMPONENT_LIB} PRIVATE
    ../features/met-weather
    ../features/environment ../features/alarm-management ../features/alarm-trigger ../features/alarm-store ../features/alarm-audio ../features/calendar-client ../features/calendar-runtime ../../assets/weather-background
    ../core)
set_source_files_properties(display_policy.c background_runtime.c weather_background.c PROPERTIES COMPILE_OPTIONS "-fstack-usage")
set_source_files_properties(../features/alarm-management/alarm_service.c ../features/alarm-management/alarm_runtime.c
    ../features/alarm-audio/alarm_bsp.c ../features/alarm-audio/alarm_player.c
    ../features/calendar-runtime/calendar_runtime_service.c PROPERTIES COMPILE_OPTIONS "-fstack-usage")
