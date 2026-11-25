# Makes "Play Splatoon.exe" in the main folder: a hard link to the game
# program (the same file under a second name, no copy). Never fails the
# build: while the game is running the old link stays.
execute_process(COMMAND "${CMAKE_COMMAND}" -E remove -f "${DST}" RESULT_VARIABLE r0 OUTPUT_QUIET ERROR_QUIET)
file(CREATE_LINK "${SRC}" "${DST}" RESULT r COPY_ON_ERROR)
if(r EQUAL 0)
    message(STATUS "Play Splatoon.exe (the game) is ready in the main folder")
else()
    message(STATUS "Play Splatoon.exe not updated (${r}); is the game running?")
endif()
