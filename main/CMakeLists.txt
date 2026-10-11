# Per-target source selection.
#
#   esp32s3 → M5PaperS3: bare EPD parallel bus ("MSG"), I2S PDM audio
#   esp32   → M5Paper  : IT8951 SPI controller, LEDC audio (no on-board speaker)
#
if (IDF_TARGET STREQUAL "esp32")
    set(PAPERBOY_EPD_IMPL  "msg/msg_it8951.c")
    set(PAPERBOY_AUDIO_PCM_IMPL_DEFAULT "audio_pwm.c")
else()
    set(PAPERBOY_EPD_IMPL  "msg/msg.c")
    set(PAPERBOY_AUDIO_PCM_IMPL_DEFAULT "audio_pdm.c")
endif()

# PCM backend is overridable:  cmake -DPAPERBOY_AUDIO_PCM_IMPL=audio_pwm.c ...
set(PAPERBOY_AUDIO_PCM_IMPL "${PAPERBOY_AUDIO_PCM_IMPL_DEFAULT}" CACHE STRING
    "PCM audio backend source file (audio_pdm.c or audio_pwm.c)")

idf_component_register(
    SRCS "main.c" "ui.c" "${PAPERBOY_EPD_IMPL}" "gbemu.c" "audio.c" "${PAPERBOY_AUDIO_PCM_IMPL}" "audio_poly_pwm.c" "minigb_apu/minigb_apu.c" "touch.c" "profiler.c" "battery.c"
    INCLUDE_DIRS "." "msg" "crankboy_core" "minigb_apu"
    REQUIRES driver esp_driver_sdspi esp_driver_sdmmc fatfs esp_timer esp_lcd esp_driver_ledc esp_driver_i2s esp_driver_i2c esp_adc
)

# Audio APU format: 16-bit signed integer
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)

if ((IDF_VERSION_MAJOR GREATER_EQUAL 5) AND (${IDF_TARGET} MATCHES "^esp32$|esp32s2|esp32s3"))
    target_compile_options(${COMPONENT_LIB} PRIVATE -fjump-tables -ftree-switch-conversion)
endif ()

# Compile the emulator translation unit at -O3.
# The CrankBoy core is embedded through gbemu.c as a single translation unit.
# Keep it highly optimised, and relax a couple of warnings that come from the
# template-expanded interpreter labels in the vendored core.
set_source_files_properties(gbemu.c PROPERTIES COMPILE_FLAGS "-O3 -Wno-error=unused-label")
