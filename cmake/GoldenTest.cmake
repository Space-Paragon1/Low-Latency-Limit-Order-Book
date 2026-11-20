# Replays INPUT through LOB_MAIN and byte-compares the outputs with the
# checked-in expectations in EXPECTED_DIR.
#
#   cmake -DLOB_MAIN=... -DINPUT=... -DEXPECTED_DIR=... -DOUT_DIR=... -P GoldenTest.cmake
#
# To accept an intentional behaviour change, copy OUT_DIR/{trades,tob}.csv over
# the files in tests/golden/ and review the diff.

foreach(var LOB_MAIN INPUT EXPECTED_DIR OUT_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "GoldenTest.cmake: ${var} is not set")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")

execute_process(
    COMMAND "${LOB_MAIN}" --quiet "${INPUT}" "${OUT_DIR}/trades.csv" "${OUT_DIR}/tob.csv"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "lob_main exited with ${rc}\n${out}\n${err}")
endif()

set(failed FALSE)
foreach(name trades.csv tob.csv)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files --ignore-eol
                "${EXPECTED_DIR}/${name}" "${OUT_DIR}/${name}"
        RESULT_VARIABLE diff)
    if(NOT diff EQUAL 0)
        message(SEND_ERROR "${name} differs from tests/golden/${name}  (actual: ${OUT_DIR}/${name})")
        set(failed TRUE)
    endif()
endforeach()

if(failed)
    message(FATAL_ERROR "golden replay mismatch")
endif()
message(STATUS "golden replay OK")
