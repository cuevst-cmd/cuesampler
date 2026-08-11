if(NOT DEFINED DST_DIR OR NOT DEFINED KEEP_NAME)
    message(FATAL_ERROR "DST_DIR and KEEP_NAME are required")
endif()

# Runtime upgrades can leave the previously bundled version beside the new one
# in an incremental build. Besides bloating the plugin, that stale nested code
# invalidates the final bundle signature because it is no longer re-signed.
file(GLOB _onnx_dylibs "${DST_DIR}/libonnxruntime.*.dylib")
foreach(_dylib IN LISTS _onnx_dylibs)
    get_filename_component(_name "${_dylib}" NAME)
    if(NOT "${_name}" STREQUAL "${KEEP_NAME}")
        file(REMOVE "${_dylib}")
        message(STATUS "Removed stale ONNX Runtime: ${_dylib}")
    endif()
endforeach()
