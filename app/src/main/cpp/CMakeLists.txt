cmake_minimum_required(VERSION 3.18)
project(mirrortv C CXX)

# Third-party sources are fetched by scripts/fetch-deps.sh into third_party/
if(NOT DEFINED THIRD_PARTY_DIR)
    get_filename_component(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party" ABSOLUTE)
endif()
set(UXPLAY_DIR   "${THIRD_PARTY_DIR}/UxPlay")
set(PLIST_DIR    "${THIRD_PARTY_DIR}/libplist")
set(ALAC_DIR     "${THIRD_PARTY_DIR}/alac/codec")
set(OPENSSL_DIR  "${THIRD_PARTY_DIR}/openssl-android/${ANDROID_ABI}")

foreach(dir UXPLAY_DIR PLIST_DIR ALAC_DIR OPENSSL_DIR)
    if(NOT EXISTS "${${dir}}")
        message(FATAL_ERROR "${dir} (${${dir}}) is missing - run scripts/fetch-deps.sh first")
    endif()
endforeach()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    # keep the decoder paths fast even in debug builds
    set(THIRD_PARTY_OPT -O2)
endif()

# ---------- OpenSSL (static libcrypto built by scripts/build-openssl.sh) ----------
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_DIR}/lib/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_DIR}/include")

# ---------- libplist (compiled straight from source, no autotools) ----------
file(GLOB PLIST_SRC "${PLIST_DIR}/src/*.c" "${PLIST_DIR}/libcnary/*.c")
add_library(plist STATIC ${PLIST_SRC})
target_include_directories(plist
    PUBLIC "${PLIST_DIR}/include"
    PRIVATE "${PLIST_DIR}/src" "${PLIST_DIR}/libcnary/include")
target_compile_definitions(plist PRIVATE
    HAVE_STRNDUP HAVE_MEMMEM HAVE_STRPTIME HAVE_LOCALTIME_R HAVE_GMTIME_R
    HAVE_TM_TM_GMTOFF HAVE_TM_TM_ZONE PACKAGE_STRING="libplist" PACKAGE_VERSION="2.7.0" LIBPLIST_STATIC)
target_compile_options(plist PRIVATE -w ${THIRD_PARTY_OPT})

# ---------- UxPlay AirPlay core ----------
file(GLOB UXPLAY_SRC "${UXPLAY_DIR}/lib/*.c")
file(GLOB PLAYFAIR_SRC "${UXPLAY_DIR}/lib/playfair/*.c")
file(GLOB LLHTTP_SRC "${UXPLAY_DIR}/lib/llhttp/*.c")
add_library(airplay STATIC
    ${UXPLAY_SRC} ${PLAYFAIR_SRC} ${LLHTTP_SRC}
    dnssd_android.c)
target_include_directories(airplay PUBLIC
    "${UXPLAY_DIR}/lib" "${UXPLAY_DIR}/lib/playfair" "${UXPLAY_DIR}/lib/llhttp"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(airplay PUBLIC
    STANDALONE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS TARGET_POSIX _LINUX _REENTRANT
    NOHOLD PLIST_210 PLIST_230 OPENSSL_API_COMPAT=0x10101000L)
target_compile_options(airplay PRIVATE -w -fPIC ${THIRD_PARTY_OPT})
target_link_libraries(airplay PUBLIC plist crypto)

# ---------- Apple ALAC decoder ----------
add_library(alac STATIC
    "${ALAC_DIR}/ALACDecoder.cpp"
    "${ALAC_DIR}/ALACBitUtilities.c"
    "${ALAC_DIR}/EndianPortable.c"
    "${ALAC_DIR}/ag_dec.c"
    "${ALAC_DIR}/dp_dec.c"
    "${ALAC_DIR}/matrix_dec.c")
target_include_directories(alac PUBLIC "${ALAC_DIR}")
target_compile_definitions(alac PRIVATE TARGET_RT_LITTLE_ENDIAN=1)
target_compile_options(alac PRIVATE -w ${THIRD_PARTY_OPT})

# ---------- the app's native library ----------
add_library(mirrortv SHARED
    airplay_jni.c
    video_decoder.c
    audio_player.c
    alac_wrapper.cpp)
target_compile_options(mirrortv PRIVATE -Wall -Wno-unused-parameter -Wno-multichar ${THIRD_PARTY_OPT})
target_link_libraries(mirrortv
    airplay alac
    android log mediandk aaudio m)
