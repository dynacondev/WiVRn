if (NOT TARGET OpenSSL::Crypto)
    # The NDK does not include OpenSSL, download it
    set(OPENSSL_VERSION "3.6.0")
    set(OPENSSL_SHA256 b6a5f44b7eb69e3fa35dbf15524405b44837a481d43d81daddde3ff21fcbb8e9)
    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl-src)
        if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz)
            if (EXISTS ${CMAKE_SOURCE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz)
                file(CREATE_LINK ${CMAKE_SOURCE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz SYMBOLIC)
            else()
                file(DOWNLOAD https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz
                     ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz
                     EXPECTED_HASH SHA256=${OPENSSL_SHA256})
            endif()
        endif()

        file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION}.tar.gz DESTINATION ${FETCHCONTENT_BASE_DIR})
        file(RENAME ${FETCHCONTENT_BASE_DIR}/openssl-${OPENSSL_VERSION} ${FETCHCONTENT_BASE_DIR}/openssl-src)
    endif()

    # NDK prebuilt binaries are host-specific. The PATH below used to hardcode
    # linux-x86_64, which breaks OpenSSL's Configure on macOS (darwin-x86_64):
    # without clang on PATH under $NDK/.../prebuilt/<host>/, Configure falls
    # through to the legacy gcc probe and dies with
    # "no NDK aarch64-linux-android-gcc on $PATH".
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(_WIVRN_NDK_HOST_TAG "darwin-x86_64")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_WIVRN_NDK_HOST_TAG "windows-x86_64")
    else()
        set(_WIVRN_NDK_HOST_TAG "linux-x86_64")
    endif()
    set(_WIVRN_NDK_TOOLCHAIN_BIN "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${_WIVRN_NDK_HOST_TAG}/bin")
    if(NOT EXISTS "${_WIVRN_NDK_TOOLCHAIN_BIN}")
        message(FATAL_ERROR "Android NDK prebuilt toolchain dir not found: ${_WIVRN_NDK_TOOLCHAIN_BIN} (host: ${CMAKE_HOST_SYSTEM_NAME})")
    endif()

    # CMAKE_BUILD_PARALLEL_LEVEL may be unset (plain `make -j` means unlimited);
    # always pass an explicit job count.
    if(DEFINED CMAKE_BUILD_PARALLEL_LEVEL AND NOT "${CMAKE_BUILD_PARALLEL_LEVEL}" STREQUAL "")
        set(_WIVRN_OPENSSL_MAKE_JOBS "${CMAKE_BUILD_PARALLEL_LEVEL}")
    elseif(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL} AND NOT "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" STREQUAL "")
        set(_WIVRN_OPENSSL_MAKE_JOBS "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
    else()
        include(ProcessorCount)
        ProcessorCount(_WIVRN_OPENSSL_MAKE_JOBS)
        if(_WIVRN_OPENSSL_MAKE_JOBS EQUAL 0)
            set(_WIVRN_OPENSSL_MAKE_JOBS 4)
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${_WIVRN_NDK_TOOLCHAIN_BIN}:$ENV{PATH} ./Configure
            android-arm64
            --prefix=${FETCHCONTENT_BASE_DIR}/openssl
            --openssldir=${FETCHCONTENT_BASE_DIR}/openssl
        WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
        OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-config-out
        ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-config-err
        RESULT_VARIABLE _WIVRN_OPENSSL_CONFIG_RES
    )
    if(NOT _WIVRN_OPENSSL_CONFIG_RES EQUAL 0)
        message(FATAL_ERROR "OpenSSL Configure for Android failed (exit ${_WIVRN_OPENSSL_CONFIG_RES}). See ${CMAKE_BINARY_DIR}/openssl-config-out and ${CMAKE_BINARY_DIR}/openssl-config-err")
    endif()

    if (NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl/lib/libcrypto.so)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${_WIVRN_NDK_TOOLCHAIN_BIN}:$ENV{PATH} make -j${_WIVRN_OPENSSL_MAKE_JOBS}
            WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
            OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-make-out
            ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-make-err
            RESULT_VARIABLE _WIVRN_OPENSSL_MAKE_RES
        )
        if(NOT _WIVRN_OPENSSL_MAKE_RES EQUAL 0)
            message(FATAL_ERROR "OpenSSL make for Android failed (exit ${_WIVRN_OPENSSL_MAKE_RES}). See ${CMAKE_BINARY_DIR}/openssl-make-out and ${CMAKE_BINARY_DIR}/openssl-make-err")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK} PATH=${_WIVRN_NDK_TOOLCHAIN_BIN}:$ENV{PATH} make install_sw
            WORKING_DIRECTORY ${FETCHCONTENT_BASE_DIR}/openssl-src
            OUTPUT_FILE ${CMAKE_BINARY_DIR}/openssl-install-out
            ERROR_FILE ${CMAKE_BINARY_DIR}/openssl-install-err
            RESULT_VARIABLE _WIVRN_OPENSSL_INSTALL_RES
        )
        if(NOT _WIVRN_OPENSSL_INSTALL_RES EQUAL 0)
            message(FATAL_ERROR "OpenSSL make install_sw for Android failed (exit ${_WIVRN_OPENSSL_INSTALL_RES}). See ${CMAKE_BINARY_DIR}/openssl-install-out and ${CMAKE_BINARY_DIR}/openssl-install-err")
        endif()
    endif()

    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl/lib/libcrypto.so OR NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl/lib/libssl.so OR NOT EXISTS ${FETCHCONTENT_BASE_DIR}/openssl/include/openssl/ssl.h)
        message(FATAL_ERROR "OpenSSL for Android was not built as expected under ${FETCHCONTENT_BASE_DIR}/openssl (missing libcrypto.so, libssl.so, or include/openssl/ssl.h)")
    endif()

    add_library(OpenSSL::Crypto STATIC IMPORTED)
    set_property(TARGET OpenSSL::Crypto PROPERTY IMPORTED_LOCATION ${FETCHCONTENT_BASE_DIR}/openssl/lib/libcrypto.so)
    target_include_directories(OpenSSL::Crypto INTERFACE ${FETCHCONTENT_BASE_DIR}/openssl/include)

    add_library(OpenSSL::SSL STATIC IMPORTED)
    set_property(TARGET OpenSSL::SSL PROPERTY IMPORTED_LOCATION ${FETCHCONTENT_BASE_DIR}/openssl/lib/libssl.so)
    target_include_directories(OpenSSL::SSL INTERFACE ${FETCHCONTENT_BASE_DIR}/openssl/include)
endif()
