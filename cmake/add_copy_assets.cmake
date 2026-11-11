# SPDX-License-Identifier: GPL-2.0-or-later

# Copy each resource independently. The old implementation appended hundreds
# of copy commands to a single custom command, which is fragile on Windows
# with MSBuild and makes failures difficult to isolate.
#
# Ensure destination directories exist: CMake's copy_if_different copies files
# but does not create arbitrary nested destination directories.
function(add_asset_copy_rule SOURCE DESTINATION)
  get_filename_component(DESTINATION_DIRECTORY "${DESTINATION}" DIRECTORY)

  add_custom_command(
    OUTPUT "${DESTINATION}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DESTINATION_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SOURCE}" "${DESTINATION}"
    DEPENDS "${SOURCE}"
    COMMENT "Copying asset ${DESTINATION}"
    VERBATIM
  )
endfunction()

function(add_copy_assets)
  file(GLOB_RECURSE RESOURCE_FILES
       RELATIVE "${CMAKE_SOURCE_DIR}/${RESOURCES_PATH}"
       "${CMAKE_SOURCE_DIR}/${RESOURCES_PATH}/*")

  # Exclude repository metadata files when copying resources.
  list(FILTER RESOURCE_FILES EXCLUDE REGEX "(.*/)*\\.git[a-z]+$")

  set(ASSET_DESTINATION_FILES "")

  foreach(RESOURCE_FILE IN LISTS RESOURCE_FILES)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${RESOURCES_PATH}/${RESOURCE_FILE}")
    set(DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${RESOURCE_COPY_PATH}/${RESOURCE_FILE}")
    add_asset_copy_rule("${SOURCE}" "${DESTINATION}")
    list(APPEND ASSET_DESTINATION_FILES "${DESTINATION}")
  endforeach()

  foreach(LICENSE_FILE IN LISTS LICENSE_FILES)
    set(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${LICENSE_FILE}")
    set(DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/doc/${LICENSE_FILE}")
    add_asset_copy_rule("${SOURCE}" "${DESTINATION}")
    list(APPEND ASSET_DESTINATION_FILES "${DESTINATION}")
  endforeach()

  set(MAIN_LICENSE_DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/doc/LICENSE")
  add_asset_copy_rule("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                      "${MAIN_LICENSE_DESTINATION}")
  list(APPEND ASSET_DESTINATION_FILES "${MAIN_LICENSE_DESTINATION}")

  add_custom_target(copy_assets ALL DEPENDS ${ASSET_DESTINATION_FILES})
  add_dependencies(dosbox copy_assets)
endfunction()
