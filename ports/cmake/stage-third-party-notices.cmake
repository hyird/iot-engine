if(NOT DEFINED DEPENDENCY_BUILD_DIR OR NOT IS_DIRECTORY "${DEPENDENCY_BUILD_DIR}")
    message(FATAL_ERROR "DEPENDENCY_BUILD_DIR must point to the configured build tree")
endif()
if(NOT DEFINED DESTINATION OR "${DESTINATION}" STREQUAL "")
    message(FATAL_ERROR "DESTINATION is required")
endif()

set(LICENSE_DESTINATION "${DESTINATION}/licenses")
file(MAKE_DIRECTORY "${LICENSE_DESTINATION}")
file(COPY_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../../THIRD_PARTY_NOTICES.md"
    "${DESTINATION}/THIRD_PARTY_NOTICES.md"
    ONLY_IF_DIFFERENT)

function(stage_license name relative_path)
    set(source "${DEPENDENCY_BUILD_DIR}/${relative_path}")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Required third-party license is missing: ${source}")
    endif()
    file(COPY_FILE "${source}" "${LICENSE_DESTINATION}/${name}.txt"
         ONLY_IF_DIFFERENT)
endfunction()

stage_license("ruvia" "_deps/ruvia-src/LICENSE")
stage_license("nanopb" "_deps/nanopb-src/LICENSE.txt")
stage_license("faac" "_deps/faac_source-src/COPYING")
stage_license("zlmediakit" "_deps/zlmediakit-src/LICENSE")
stage_license("zltoolkit" "_deps/zlmediakit-src/3rdpart/ZLToolKit/LICENSE")
stage_license("jsoncpp" "_deps/zlmediakit-src/3rdpart/jsoncpp/LICENSE")

file(GLOB vcpkg_copyrights
    LIST_DIRECTORIES FALSE
    "${DEPENDENCY_BUILD_DIR}/vcpkg_installed/*/share/*/copyright")
if(NOT vcpkg_copyrights)
    message(FATAL_ERROR "No vcpkg license files found in ${DEPENDENCY_BUILD_DIR}")
endif()
foreach(copyright IN LISTS vcpkg_copyrights)
    get_filename_component(package_directory "${copyright}" DIRECTORY)
    get_filename_component(package_name "${package_directory}" NAME)
    file(COPY_FILE "${copyright}"
         "${LICENSE_DESTINATION}/vcpkg-${package_name}.txt"
         ONLY_IF_DIFFERENT)
endforeach()
