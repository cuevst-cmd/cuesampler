# Applies the local Bungee patch (Stream::reset() / InputBuffer::reset()) to the
# fetched Bungee source tree, idempotently. Invoked by FetchContent's
# PATCH_COMMAND via `cmake -P`, with -DSRC_DIR=<bungee source> and
# -DPATCH_FILE=<path to .patch>. Safe to run more than once: if the patch is
# already applied it is detected and skipped rather than failing the build.
#
# Upstream Bungee (commit 7354c0c) has no Stream::reset(); the plugin requires
# it (PluginProcessor.cpp, bungeeResetPending path). Keeping the change as a
# patch lets us pin pristine upstream and stay reproducible on macOS + Windows.

if(NOT DEFINED SRC_DIR OR NOT DEFINED PATCH_FILE)
    message(FATAL_ERROR "apply_bungee_patch: SRC_DIR and PATCH_FILE must be set")
endif()

# Can the patch be applied as-is?
execute_process(
    COMMAND git apply --ignore-whitespace --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SRC_DIR}"
    RESULT_VARIABLE can_apply
    ERROR_QUIET OUTPUT_QUIET)

if(can_apply EQUAL 0)
    execute_process(
        COMMAND git apply --ignore-whitespace "${PATCH_FILE}"
        WORKING_DIRECTORY "${SRC_DIR}"
        RESULT_VARIABLE applied)
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR "apply_bungee_patch: failed to apply ${PATCH_FILE}")
    endif()
    message(STATUS "Bungee: applied Stream::reset() patch")
else()
    # Already applied? The reverse patch should then apply cleanly.
    execute_process(
        COMMAND git apply --ignore-whitespace --reverse --check "${PATCH_FILE}"
        WORKING_DIRECTORY "${SRC_DIR}"
        RESULT_VARIABLE already_applied
        ERROR_QUIET OUTPUT_QUIET)
    if(already_applied EQUAL 0)
        message(STATUS "Bungee: Stream::reset() patch already applied — skipping")
    else()
        message(FATAL_ERROR
            "apply_bungee_patch: ${PATCH_FILE} neither applies nor is already "
            "applied to ${SRC_DIR}. The pinned Bungee commit may have changed.")
    endif()
endif()
