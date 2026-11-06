# Makes the release zip, next to the exe. The build runs this with:
#   CONFIG        The configuration being built. Only Release gets a zip.
#   EXE           The tool.
#   SOURCE_DIR    The root of the repository.
#   STAGE_DIR     An empty folder to assemble the zip in. Anything already in it is removed.
#   PACKAGE_NAME  The name of the zip, and of the folder inside it.

if(NOT CONFIG STREQUAL "Release")
	return()
endif()

cmake_path(GET EXE PARENT_PATH output_dir)
set(zip "${output_dir}/${PACKAGE_NAME}.zip")
set(stage "${STAGE_DIR}/${PACKAGE_NAME}")

# Removed first, so that if anything below fails, an old zip can't pass for this build's.
file(REMOVE "${zip}")
file(REMOVE_RECURSE "${STAGE_DIR}")

# Everything the README links to has to be here, or its links are dead once unzipped.
file(COPY
	"${EXE}"
	"${SOURCE_DIR}/README.md"
	"${SOURCE_DIR}/LICENSE"
	"${SOURCE_DIR}/DESIGN.md"
	"${SOURCE_DIR}/docs"
	DESTINATION "${stage}")
file(COPY "${SOURCE_DIR}/assets/icon.svg" DESTINATION "${stage}/assets")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip "${PACKAGE_NAME}"
	WORKING_DIRECTORY "${STAGE_DIR}"
	COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "Packaged ${zip}")
