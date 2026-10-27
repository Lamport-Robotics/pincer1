# Pincer1 consent controller firmware (Raspberry Pi Pico 2 / RP2350, Arm secure mode) + host unit tests.
#
#   Target build : cmake -S firmware -B <dir> -DPICO_TOOLCHAIN_PATH=… && cmake --build <dir>   -> pincer1_consent.uf2
#   Host tests   : cmake -S firmware -B <dir> -DBUILD_HOST_TESTS=ON && cmake --build <dir> && ctest --test-dir <dir>
#
# Options:
#   BUILD_HOST_TESTS      compile the pure-logic modules + tests with the host compiler (no pico-sdk)
#   PINCER_SOFT_SIGNER    software ed25519 signer instead of the SE050 (dev boards; hello reports se050:false)
#   PINCER_SIGN_KEY       path to an EC P-256 private key PEM; when set the image is signed and hashed for RP2350
#                         secure boot (pico_sign_binary / pico_hash_binary) and <build>/pincer1_consent.otp.json is
#                         written with the boot-key hash for scripts/provision-otp.sh
#   PINCER_DEV_SCP03_KEYS 96 hex chars (ENC||MAC||DEK) compiled in as the SE050 Platform SCP03 keys for bench
#                         work; used only when the OTP rows are blank. Never set this for a production image.
#   PINCER_NO_DISPLAY     bench boards without the SSD1309 panel: the display driver's bus calls become no-ops
#                         (every screen is still rendered and the consent machine still requires the "shown"
#                         step), hello reports display:false. PRODUCTION IMAGES MUST NOT SET THIS: without the
#                         panel the owner has no trusted view of what is being signed.
#   PICO_BOARD            pico2 (default). pico (RP2040) still configures and builds but has no OTP/secure boot.
cmake_minimum_required(VERSION 3.13)

option(BUILD_HOST_TESTS "Build host unit tests instead of the firmware" OFF)
option(PINCER_SOFT_SIGNER "Use the software ed25519 signer (no SE050)" OFF)
option(PINCER_NO_DISPLAY "Bench boards without the trusted display (never for production)" OFF)
set(PINCER_SIGN_KEY "" CACHE FILEPATH "EC P-256 private key PEM used to sign the RP2350 image (empty = unsigned)")
set(PINCER_DEV_SCP03_KEYS "" CACHE STRING "Bench-only SCP03 keys, 96 hex chars ENC||MAC||DEK (empty = OTP only)")

set(PINCER_CORE_SOURCES
    src/util.c
    src/sha256.c
    src/hmac_sha512.c
    src/keygen.c
    src/solmsg.c
    src/policy.c
    src/consent.c
    src/storage.c
    src/update.c
    src/touch.c
    src/proto.c
    src/aes128.c
    src/scp03.c
    src/u256.c
    src/keccak.c
    src/ripemd160.c
    src/secp256k1.c
    src/bip32.c
    src/chainaddr.c
    src/evmtx.c
    src/eip712.c
    src/btcpsbt.c
    src/routers.c
    src/display.c
    src/trusted_ui.c
    third_party/cjson/cJSON.c
    third_party/ed25519/src/sha512.c
    third_party/ed25519/src/fe.c
    third_party/ed25519/src/ge.c
    third_party/micro-ecc/uECC.c
    third_party/tiny_sha3/sha3.c
    third_party/bech32/segwit_addr.c
)
# micro-ecc: secp256k1 only, VLI/point API exposed for src/secp256k1.c (RFC 6979 sign, recovery).
set(PINCER_UECC_DEFINES
    uECC_SUPPORTS_secp160r1=0
    uECC_SUPPORTS_secp192r1=0
    uECC_SUPPORTS_secp224r1=0
    uECC_SUPPORTS_secp256r1=0
    uECC_SUPPORTS_secp256k1=1
    uECC_SUPPORT_COMPRESSED_POINT=1
    uECC_ENABLE_VLI_API=1
    uECC_OPTIMIZATION_LEVEL=2
)
set(PINCER_ED25519_SOURCES
    third_party/ed25519/src/add_scalar.c
    third_party/ed25519/src/key_exchange.c
    third_party/ed25519/src/keypair.c
    third_party/ed25519/src/sc.c
    third_party/ed25519/src/sign.c
    third_party/ed25519/src/verify.c
)
set(PINCER_CORE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ed25519/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bip39
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/micro-ecc
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tiny_sha3
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/bech32
)

if(BUILD_HOST_TESTS)
    project(pincer1_consent_tests C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    enable_testing()

    add_library(pincer_core STATIC ${PINCER_CORE_SOURCES} ${PINCER_ED25519_SOURCES} src/signer_soft.c)
    target_include_directories(pincer_core PUBLIC ${PINCER_CORE_INCLUDES})
    target_compile_definitions(pincer_core PUBLIC ED25519_NO_SEED=1 CJSON_NESTING_LIMIT=32 ${PINCER_UECC_DEFINES})
    target_compile_options(pincer_core PRIVATE -Wall -Wextra)
    set_source_files_properties(third_party/micro-ecc/uECC.c third_party/bech32/segwit_addr.c
                                PROPERTIES COMPILE_OPTIONS "-w") # vendored as-is

    foreach(t util proto touch storage update solmsg policy consent keygen scp03 inheritance secp256k1 evmtx btcpsbt display trusted_ui eip712)
        add_executable(test_${t} test/test_${t}.c)
        target_link_libraries(test_${t} pincer_core)
        target_include_directories(test_${t} PRIVATE test)
        target_compile_options(test_${t} PRIVATE -Wall -Wextra -Wno-unused-function)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()

    # secrets.c is compiled into the test itself so the dev-key fallback can be exercised with a known string.
    add_executable(test_secrets test/test_secrets.c src/secrets.c)
    target_link_libraries(test_secrets pincer_core)
    target_include_directories(test_secrets PRIVATE test)
    target_compile_definitions(test_secrets PRIVATE
        PINCER_DEV_SCP03_KEYS="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f")
    target_compile_options(test_secrets PRIVATE -Wall -Wextra -Wno-unused-function)
    add_test(NAME secrets COMMAND test_secrets)
else()
    # ---- firmware ----
    set(PICO_BOARD pico2 CACHE STRING "Board type (pico2 = RP2350; pico = RP2040, no secure boot / OTP)")
    if(PICO_BOARD STREQUAL "pico2" AND NOT DEFINED PICO_PLATFORM AND NOT DEFINED ENV{PICO_PLATFORM})
        set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform (Arm Cortex-M33, secure mode)")
    endif()
    if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT DEFINED PICO_SDK_PATH)
        set(PICO_SDK_FETCH_FROM_GIT ON)
    endif()
    include(pico_sdk_import.cmake)
    if(PICO_SDK_VERSION_MAJOR LESS 2)
        message(FATAL_ERROR "pico-sdk ${PICO_SDK_VERSION_STRING} is too old: RP2350 needs a 2.x SDK")
    endif()

    file(STRINGS src/proto.h PINCER_FW_VERSION_LINE REGEX "define PROTO_FW_VERSION")
    string(REGEX REPLACE ".*\"([0-9]+)\\.([0-9]+)\\.[0-9]+\".*" "\\1" PINCER_FW_MAJOR "${PINCER_FW_VERSION_LINE}")
    string(REGEX REPLACE ".*\"[0-9]+\\.([0-9]+)\\.[0-9]+\".*" "\\1" PINCER_FW_MINOR "${PINCER_FW_VERSION_LINE}")
    project(pincer1_consent C CXX ASM)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
    pico_sdk_init()

    add_executable(pincer1_consent
        src/main.c
        src/rng.c
        src/secrets.c
        ${PINCER_CORE_SOURCES}
        ${PINCER_ED25519_SOURCES}
    )
    target_include_directories(pincer1_consent PRIVATE ${PINCER_CORE_INCLUDES})
    target_compile_definitions(pincer1_consent PRIVATE
        ED25519_NO_SEED=1
        CJSON_NESTING_LIMIT=32      # a request is a few levels deep; the parser recurses per level
        PICO_STACK_SIZE=0x1000       # explicit, the whole of SCRATCH_Y; the parsers work in static scratch, not on the stack
        ${PINCER_UECC_DEFINES}
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
        PICO_FLASH_ASSUME_CORE1_SAFE=1
    )
    target_compile_options(pincer1_consent PRIVATE -Wall -Wextra -Wno-unused-parameter)
    set_source_files_properties(third_party/micro-ecc/uECC.c third_party/bech32/segwit_addr.c
                                PROPERTIES COMPILE_OPTIONS "-w") # vendored as-is

    if(PINCER_DEV_SCP03_KEYS)
        string(LENGTH "${PINCER_DEV_SCP03_KEYS}" _devlen)
        if(NOT _devlen EQUAL 96)
            message(FATAL_ERROR "PINCER_DEV_SCP03_KEYS must be exactly 96 hex characters (ENC||MAC||DEK)")
        endif()
        target_compile_definitions(pincer1_consent PRIVATE PINCER_DEV_SCP03_KEYS="${PINCER_DEV_SCP03_KEYS}")
        message(WARNING "pincer1_consent: bench SCP03 keys compiled into the image (PINCER_DEV_SCP03_KEYS). Not for production.")
    endif()

    if(PINCER_NO_DISPLAY)
        target_compile_definitions(pincer1_consent PRIVATE PINCER_NO_DISPLAY=1)
        message(WARNING "pincer1_consent: built WITHOUT the trusted display (PINCER_NO_DISPLAY). Bench only, never for production.")
    endif()

    if(PINCER_SOFT_SIGNER)
        target_sources(pincer1_consent PRIVATE src/signer_soft.c)
        target_compile_definitions(pincer1_consent PRIVATE PINCER_SOFT_SIGNER=1)
    else()
        # NXP Plug & Trust (git submodule). Only the T1oI2C transport + SE05x APDU layer is compiled; the SSS
        # abstraction layer and its host-crypto back-ends are not used. Platform SCP03 is implemented in
        # src/scp03.c (same wire format as the middleware, see that header) over src/aes128.c.
        set(PT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/plug-and-trust)
        if(NOT EXISTS ${PT}/hostlib/hostLib/se05x_03_xx_xx/se05x_APDU.c)
            message(FATAL_ERROR "plug-and-trust submodule missing: run `git submodule update --init` (or build with -DPINCER_SOFT_SIGNER=ON)")
        endif()
        target_sources(pincer1_consent PRIVATE
            src/signer_se050.c
            src/se050_port.c
            ${PT}/hostlib/hostLib/se05x_03_xx_xx/se05x_APDU.c
            ${PT}/hostlib/hostLib/se05x/src/se05x_tlv.c
            ${PT}/hostlib/hostLib/se05x/src/se05x_ECC_curves.c
            ${PT}/hostlib/hostLib/libCommon/smCom/smCom.c
            ${PT}/hostlib/hostLib/libCommon/infra/global_platf.c
            ${PT}/hostlib/hostLib/libCommon/smCom/smComT1oI2C.c
            ${PT}/hostlib/hostLib/libCommon/smCom/T1oI2C/phNxpEse_Api.c
            ${PT}/hostlib/hostLib/libCommon/smCom/T1oI2C/phNxpEsePal_i2c.c
            ${PT}/hostlib/hostLib/libCommon/smCom/T1oI2C/phNxpEseProto7816_3.c
        )
        target_include_directories(pincer1_consent PRIVATE
            src/compat
            ${PT}
            ${PT}/sss/inc
            ${PT}/sss/port/default
            ${PT}/hostlib/hostLib/inc
            ${PT}/hostlib/hostLib/libCommon/infra
            ${PT}/hostlib/hostLib/libCommon/smCom
            ${PT}/hostlib/hostLib/libCommon/smCom/T1oI2C
            ${PT}/hostlib/hostLib/libCommon/log
            ${PT}/hostlib/hostLib/se05x_03_xx_xx
            ${PT}/hostlib/hostLib/platform/inc
        )
        target_compile_definitions(pincer1_consent PRIVATE
            SSS_USE_FTR_FILE=1
            SMCOM_T1oI2C=1
            T1oI2C=1
            T1oI2C_UM11225=1
            T1OI2C_RETRY_ON_I2C_FAILED=1
            AX_EMBEDDED=1  # embedded build: no pthread; fsl_debug_console.h comes from src/compat
            NX_LOG_ENABLE_DEFAULT_DEBUG=0
        )
        target_link_libraries(pincer1_consent hardware_i2c)
    endif()

    target_link_libraries(pincer1_consent
        pico_stdlib
        pico_rand
        pico_unique_id
        pico_bootrom
        hardware_flash
        hardware_sync
        hardware_adc
        hardware_spi
        hardware_watchdog
        pico_bootrom
    )
    pico_enable_stdio_usb(pincer1_consent 1)
    pico_enable_stdio_uart(pincer1_consent 0)

    # ---- RP2350 secure boot: sign + hash the image (SDK: tools/CMakeLists.txt pico_sign_binary / pico_hash_binary /
    # pico_set_otp_key_output_file; picotool seal does the work after link). The OTP JSON it writes carries the
    # public-key hash (BOOTKEY0) and the key-valid flag; scripts/provision-otp.sh burns it. ----
    if(PINCER_SIGN_KEY)
        if(NOT PICO_RP2350)
            message(FATAL_ERROR "PINCER_SIGN_KEY: signed images need PICO_BOARD=pico2 (RP2350); the RP2040 has no secure boot")
        endif()
        if(NOT EXISTS "${PINCER_SIGN_KEY}")
            message(FATAL_ERROR "PINCER_SIGN_KEY='${PINCER_SIGN_KEY}' does not exist (scripts/keygen.sh creates one)")
        endif()
        pico_set_otp_key_output_file(pincer1_consent ${CMAKE_BINARY_DIR}/pincer1_consent.otp.json)
        pico_sign_binary(pincer1_consent ${PINCER_SIGN_KEY})
        pico_hash_binary(pincer1_consent)
        target_compile_definitions(pincer1_consent PRIVATE PINCER_SIGNED_IMAGE=1)
        message(STATUS "pincer1_consent: image will be signed with ${PINCER_SIGN_KEY}; OTP rows -> ${CMAKE_BINARY_DIR}/pincer1_consent.otp.json")
    elseif(PICO_RP2350)
        message(STATUS "pincer1_consent: UNSIGNED image (set -DPINCER_SIGN_KEY=firmware/keys/pincer1-boot.pem for secure boot)")
    endif()

    # ---- A/B partitions + image version (field updates, docs/PROTOCOL.md "update_*"): the partition table
    # is embedded in the image and written with it; the bootrom boots the newer valid image of the pair and
    # the firmware stages the next one into the spare partition. Storage and the trial ledger live in the
    # unpartitioned tail. ----
    if(PICO_RP2350)
        pico_embed_pt_in_binary(pincer1_consent ${CMAKE_CURRENT_SOURCE_DIR}/pt/pincer1.json)
        pico_set_binary_version(pincer1_consent MAJOR ${PINCER_FW_MAJOR} MINOR ${PINCER_FW_MINOR})
    endif()

    pico_add_extra_outputs(pincer1_consent)
endif()
