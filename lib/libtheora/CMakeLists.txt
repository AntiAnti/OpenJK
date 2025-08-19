cmake_minimum_required(VERSION 3.10)
project(bundled_libtheora C)

set(CMAKE_C_STANDARD 99)

set(THEORA_SOURCES
    lib/analyze.c
    lib/fdct.c
    lib/encfrag.c
    lib/encapiwrapper.c
    lib/encinfo.c
    lib/encode.c
    lib/enquant.c
    lib/huffenc.c
    lib/mathops.c
    lib/mcenc.c
    lib/rate.c
    lib/tokenize.c
    lib/encoder_disabled.c

    lib/apiwrapper.c
    lib/bitpack.c
    lib/dequant.c
    lib/fragment.c
    lib/idct.c
    lib/info.c
    lib/internal.c
    lib/quant.c
    lib/state.c

    lib/decapiwrapper.c
    lib/decinfo.c
    lib/decode.c
    lib/huffdec.c
)

add_library(bundled_libtheora STATIC ${THEORA_SOURCES})

target_include_directories(bundled_libtheora PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../libogg/include
)

if (MSVC)
    target_compile_definitions(bundled_libtheora PRIVATE
        _CRT_SECURE_NO_DEPRECATE
        _CRT_NONSTDC_NO_DEPRECATE
        _BIND_TO_CURRENT_CRT_VERSION
    )
endif()

target_link_libraries(bundled_libtheora PRIVATE bundled_libogg)