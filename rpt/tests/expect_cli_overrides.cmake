# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
if(NOT DEFINED PROGRAM OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "PROGRAM and CONFIG are required")
endif()

file(SHA256 "${CONFIG}" config_hash_before)

execute_process(
    COMMAND "${PROGRAM}"
        --config "${CONFIG}"
        --uhd-device "type=b200,serial=cli-contract-test"
        --rx-frequency 145400000
        --tx-frequency 438500000
    RESULT_VARIABLE override_result
    OUTPUT_VARIABLE override_output
    ERROR_VARIABLE override_error)

set(override_combined "${override_output}${override_error}")
if(NOT override_result EQUAL 3 OR
   NOT override_combined MATCHES "MissingVectorSet")
    message(FATAL_ERROR
        "UHD/RX/TX startup overrides were not accepted as a validated T2 startup.\n${override_combined}")
endif()

execute_process(
    COMMAND "${PROGRAM}"
        --config "${CONFIG}"
        --args "type=b200,serial=cli-alias-test"
    RESULT_VARIABLE alias_result
    OUTPUT_VARIABLE alias_output
    ERROR_VARIABLE alias_error)

set(alias_combined "${alias_output}${alias_error}")
if(NOT alias_result EQUAL 3 OR
   NOT alias_combined MATCHES "MissingVectorSet")
    message(FATAL_ERROR
        "--args was not accepted as the UHD device-parameter alias.\n${alias_combined}")
endif()

file(READ "${CONFIG}" disabled_fm_config)
string(REPLACE "ctcss: { required: true"
    "ctcss: { required: false"
    disabled_fm_config "${disabled_fm_config}")
get_filename_component(program_directory "${PROGRAM}" DIRECTORY)
set(invalid_fm_config "${program_directory}/dmr_b210_cli_disabled_fm.yaml")
file(WRITE "${invalid_fm_config}" "${disabled_fm_config}")

execute_process(
    COMMAND "${PROGRAM}" --config "${invalid_fm_config}"
    RESULT_VARIABLE without_disable_result
    OUTPUT_VARIABLE without_disable_output
    ERROR_VARIABLE without_disable_error)
if(NOT without_disable_result EQUAL 1)
    file(REMOVE "${invalid_fm_config}")
    message(FATAL_ERROR
        "Invalid enabled FM configuration unexpectedly started.\n${without_disable_output}${without_disable_error}")
endif()

execute_process(
    COMMAND "${PROGRAM}" --config "${invalid_fm_config}" --disable-fm
    RESULT_VARIABLE disabled_result
    OUTPUT_VARIABLE disabled_output
    ERROR_VARIABLE disabled_error)
file(REMOVE "${invalid_fm_config}")

set(disabled_combined "${disabled_output}${disabled_error}")
if(NOT disabled_result EQUAL 3 OR
   NOT disabled_combined MATCHES "MissingVectorSet")
    message(FATAL_ERROR
        "--disable-fm did not suppress invalid enabled-FM validation.\n${disabled_combined}")
endif()

foreach(removed_option IN ITEMS
        --dry-run
        --self-test
        --rx-diagnostic
        --vector-root
        --run-seconds
        --sms-text)
    execute_process(
        COMMAND "${PROGRAM}" --config "${CONFIG}" "${removed_option}" rejected
        RESULT_VARIABLE removed_result
        OUTPUT_VARIABLE removed_output
        ERROR_VARIABLE removed_error)
    if(removed_result EQUAL 0)
        message(FATAL_ERROR
            "Removed command-line option was accepted: ${removed_option}\n${removed_output}${removed_error}")
    endif()
endforeach()

foreach(invalid_frequency IN ITEMS 0 -1 145400000.5 abc 135999999)
    execute_process(
        COMMAND "${PROGRAM}" --config "${CONFIG}"
            --rx-frequency "${invalid_frequency}"
        RESULT_VARIABLE invalid_result
        OUTPUT_VARIABLE invalid_output
        ERROR_VARIABLE invalid_error)
    if(invalid_result EQUAL 0)
        message(FATAL_ERROR
            "Invalid RX frequency was accepted: ${invalid_frequency}\n${invalid_output}${invalid_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${PROGRAM}" --config "${CONFIG}" --unknown-option value
    RESULT_VARIABLE unknown_result
    OUTPUT_VARIABLE unknown_output
    ERROR_VARIABLE unknown_error)
if(unknown_result EQUAL 0)
    message(FATAL_ERROR
        "Unknown command-line option was accepted.\n${unknown_output}${unknown_error}")
endif()

file(SHA256 "${CONFIG}" config_hash_after)
if(NOT config_hash_before STREQUAL config_hash_after)
    message(FATAL_ERROR "Command-line overrides modified the YAML file")
endif()
