add_subdirectory(core)
add_subdirectory(backend)
add_subdirectory(worker)
add_subdirectory(utils)
add_subdirectory(ui)
# add_subdirectory(libs/bled)  # TODO: port bled library properly

add_executable(rufus main.cpp)
target_link_libraries(rufus PRIVATE
    rufus_core
    rufus_backend
    rufus_worker
    rufus_ui
    rufus_utils
    Qt6::Core
    Qt6::Widgets
)

# lupdate target: extract translatable strings into res/translations/*.ts
file(GLOB TS_FILES_ALL ${CMAKE_SOURCE_DIR}/res/translations/rufus_*.ts)
if(TS_FILES_ALL AND TARGET Qt6::lupdate)
    add_custom_target(lupdate
        COMMAND $<TARGET_FILE:Qt6::lupdate> ${CMAKE_SOURCE_DIR}/src
            -ts ${TS_FILES_ALL}
        COMMENT "Updating translation files (res/translations/*.ts)"
    )
endif()

# Qt resource file (old-style qt6_add_resources: the target-style form is
# a no-op with Qt 6.11/CMake 4, so generate the .cpp and add it manually)
set(RUFUS_RCC_SOURCES "")
qt6_add_resources(RUFUS_RCC_SOURCES
    "${CMAKE_SOURCE_DIR}/res/resources.qrc"
)
target_sources(rufus PRIVATE ${RUFUS_RCC_SOURCES})

# Generate .qm files for all .ts files
file(GLOB TS_FILES ${CMAKE_SOURCE_DIR}/res/translations/rufus_*.ts)

set(QM_FILES "")
foreach(ts_file ${TS_FILES})
    get_filename_component(ts_name ${ts_file} NAME)
    string(REPLACE ".ts" ".qm" qm_name ${ts_name})
    set(qm_file "${CMAKE_BINARY_DIR}/src/${qm_name}")

    add_custom_command(
        OUTPUT ${qm_file}
        COMMAND $<TARGET_FILE:Qt6::lrelease> ${ts_file} -qm ${qm_file}
        DEPENDS ${ts_file}
        COMMENT "Generating ${qm_name}"
    )
    list(APPEND QM_FILES ${qm_file})
endforeach()

# Custom target to build all .qm files
add_custom_target(rufus_qm ALL DEPENDS ${QM_FILES})

# Add dependency so .qm are built before linking
add_dependencies(rufus rufus_qm)

# Copy .qm files alongside the binary. This must run whenever a .qm is
# regenerated (not only when the binary relinks), since Localization loads
# translations from <appDir>/translations at runtime.
set(QM_COPY_FILES "")
foreach(qm_file ${QM_FILES})
    get_filename_component(qm_name ${qm_file} NAME)
    set(copy_output "${CMAKE_BINARY_DIR}/src/translations/${qm_name}")
    add_custom_command(
        OUTPUT ${copy_output}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/src/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${qm_file}" "${copy_output}"
        DEPENDS ${qm_file}
        COMMENT "Copying ${qm_name} to bin/translations"
    )
    list(APPEND QM_COPY_FILES ${copy_output})
endforeach()
add_custom_target(rufus_qm_copies ALL DEPENDS ${QM_COPY_FILES})
add_dependencies(rufus rufus_qm_copies)

# Copy .qm files back to source at build time
foreach(qm_file ${QM_FILES})
    get_filename_component(qm_name ${qm_file} NAME)
    add_custom_command(TARGET rufus POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/res/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${qm_file}" "${CMAKE_SOURCE_DIR}/res/translations/${qm_name}"
        COMMENT "Copying ${qm_name} to res/translations"
    )
endforeach()

# Copy runtime resources (FreeDOS, UEFI:NTFS, MBR templates, syslinux)
# alongside the binary so bootloader features work from a build tree too.
set(RUNTIME_RESOURCES
    freedos
    mbr
    syslinux
)
foreach(res_dir ${RUNTIME_RESOURCES})
    add_custom_command(TARGET rufus POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/res/${res_dir}"
            "${CMAKE_BINARY_DIR}/src/${res_dir}"
        COMMENT "Copying res/${res_dir} to binary directory"
    )
endforeach()
add_custom_command(TARGET rufus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/res/uefi/uefi-ntfs.img"
        "${CMAKE_BINARY_DIR}/src/uefi-ntfs.img"
    COMMENT "Copying uefi-ntfs.img to binary directory"
)

install(TARGETS rufus RUNTIME DESTINATION bin)
install(FILES ${QM_FILES} DESTINATION bin/translations)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/res/freedos DESTINATION share/rufus)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/res/mbr DESTINATION share/rufus)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/res/syslinux DESTINATION share/rufus)
install(FILES ${CMAKE_SOURCE_DIR}/res/uefi/uefi-ntfs.img
    DESTINATION share/rufus)
install(FILES ${CMAKE_SOURCE_DIR}/res/ie.rufus.rufus.policy
    DESTINATION share/polkit-1/actions)
install(FILES ${CMAKE_SOURCE_DIR}/res/ie.rufus.rufus.desktop
    DESTINATION share/applications)
install(FILES ${CMAKE_SOURCE_DIR}/res/icons/rufus-512.png DESTINATION share/icons/hicolor/512x512/apps RENAME rufus.png)
