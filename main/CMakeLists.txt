set(PAPERBOY_AUDIO_PCM_IMPL "audio_pdm.c" CACHE STRING
    "PCM audio backend source file (audio_pdm.c or audio_pwm.c)")

idf_component_register(
    SRCS
        "main.c"
        "ui.c"
        "msg/msg.c"
        "gbemu.c"
        "gbemu_api.c"
        "gbemu_jit.c"
        "audio.c"
        "${PAPERBOY_AUDIO_PCM_IMPL}"
        "audio_poly_pwm.c"
        "minigb_apu/minigb_apu.c"
        "touch.c"
        "profiler.c"
        "battery.c"
        "gbjit-xtensa/core/memory.c"
        "gbjit-xtensa/core/ppu.c"
        "gbjit-xtensa/core/ppu_thread.c"
        "gbjit-xtensa/core/sm83_decoder.c"
        "gbjit-xtensa/core/sm83_interp.c"
        "gbjit-xtensa/jit/emit_xtensa.c"
        "gbjit-xtensa/jit/codecache.c"
        "gbjit-xtensa/jit/codegen.c"
        "gbjit-xtensa/jit/dispatcher.c"
        "gbjit-xtensa/port/esp32s3/components/gbjit/jit_trampolines.S"
    INCLUDE_DIRS "." "msg" "minigb_apu" "gbjit-xtensa/include" "gbjit-xtensa/core" "gbjit-xtensa/jit"
    REQUIRES driver esp_driver_sdspi esp_driver_sdmmc fatfs esp_timer esp_lcd esp_driver_ledc esp_driver_i2s esp_driver_i2c esp_adc freertos heap
)

# Audio APU format: 16-bit signed integer
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)

# if ((IDF_VERSION_MAJOR GREATER_EQUAL 5) AND (${IDF_TARGET} MATCHES "^esp32$|esp32s2|esp32s3"))
#     target_compile_options(${COMPONENT_LIB} PRIVATE -fjump-tables -ftree-switch-conversion)
# endif ()

# Compile the emulator translation unit at -O3.
# walnut_cgb.h is included here and contains the entire SM83 CPU decoder;
# the extra optimisation (better register allocation, unrolling) typically
# saves 5–15% on the hot loop.
# target_compile_definitions(${COMPONENT_LIB} PRIVATE DEBUG=0 GBJIT_DISPATCHER_CHAIN_BATCH=4)

set_source_files_properties(gbemu.c gbemu_jit.c PROPERTIES COMPILE_FLAGS "-O3")
