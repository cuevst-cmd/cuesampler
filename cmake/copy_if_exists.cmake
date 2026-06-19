# copy_if_exists.cmake
# ---------------------
# Copies SRC -> DST only if SRC exists, creating DST's parent directory first.
#
# Used by the POST_BUILD bundling of the HTDemucs-FT stem models. Those ONNX
# files are ~316 MB each — far over GitHub's 100 MB file limit — so they are NOT
# committed (see .gitignore) and are absent on a clean checkout / CI runner. A
# missing model must NOT fail the build: the plugin runs fine without stem
# separation (StemSeparator falls back to the original buffer). This mirrors the
# `-P` helper-script pattern already used for patches/apply_bungee_patch.cmake.
#
# Invoked as:
#   ${CMAKE_COMMAND} -DSRC=<abs src> -DDST=<abs dst> -P cmake/copy_if_exists.cmake

if(EXISTS "${SRC}")
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}"
                    RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
        message(STATUS "Bundled stem model: ${DST}")
    else()
        message(WARNING "Failed to copy stem model: ${SRC} -> ${DST}")
    endif()
else()
    message(STATUS "Stem model absent, skipped (run export_htdemucs.py): ${SRC}")
endif()
