# Compiled and written by BG1KK.
# Privatization and closed-source use are strictly forbidden.
# GNU Radio components are copyrighted by their respective developers.
# All other code copyright © BG1KK.
# This copyright statement must be retained.
if(NOT DEFINED PROGRAM)
    message(FATAL_ERROR "PROGRAM is required")
endif()

execute_process(
    COMMAND "${PROGRAM}" --help
    RESULT_VARIABLE short_result
    OUTPUT_VARIABLE short_output
    ERROR_VARIABLE short_error)

if(NOT short_result EQUAL 0)
    message(FATAL_ERROR
        "--help failed with ${short_result}\nstdout:\n${short_output}\nstderr:\n${short_error}")
endif()

set(required_short
    "--config"
    "--uhd-device"
    "--args"
    "--rx-frequency"
    "--tx-frequency"
    "--disable-fm"
    "--help-detail"
    "--version")
foreach(token IN LISTS required_short)
    string(FIND "${short_output}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "--help is missing ${token}\n${short_output}")
    endif()
endforeach()

set(removed_options
    "--dry-run"
    "--self-test"
    "--rx-diagnostic"
    "--vector-root"
    "--run-seconds"
    "--sms-text")
foreach(token IN LISTS removed_options)
    string(FIND "${short_output}" "${token}" position)
    if(NOT position EQUAL -1)
        message(FATAL_ERROR "--help still exposes removed option ${token}\n${short_output}")
    endif()
endforeach()

execute_process(
    COMMAND "${PROGRAM}" --help-detail
    RESULT_VARIABLE detailed_result
    OUTPUT_VARIABLE detailed_output
    ERROR_VARIABLE detailed_error)

if(NOT detailed_result EQUAL 0)
    message(FATAL_ERROR
        "--help-detail failed with ${detailed_result}\nstdout:\n${detailed_output}\nstderr:\n${detailed_error}")
endif()

set(required_definitions
    "UHD = USRP Hardware Driver"
    "USRP = Universal Software Radio Peripheral"
    "DMR = Digital Mobile Radio"
    "RX = Receive"
    "TX = Transmit"
    "FM = Frequency Modulation"
    "Hz = Hertz"
    "YAML = YAML Ain't Markup Language")
foreach(definition IN LISTS required_definitions)
    string(FIND "${detailed_output}" "${definition}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "--help-detail is missing abbreviation definition '${definition}'\n${detailed_output}")
    endif()
endforeach()

execute_process(
    COMMAND "${PROGRAM}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error)
if(NOT version_result EQUAL 0 OR
   NOT version_output MATCHES "V[0-9]+\\.[0-9]+\\.[0-9]+ B[0-9]+")
    message(FATAL_ERROR
        "--version did not return release and build sequence.\n${version_output}${version_error}")
endif()
