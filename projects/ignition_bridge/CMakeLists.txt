cmake_minimum_required(VERSION 3.16)

project(ignition_bridge)

add_library(ignition_bridge_objs OBJECT
    mapped_event_circular_buffer.cpp
    mapped_event_circular_buffer_c.cpp
    shared_memory_block_c.cpp
    time_c.cpp
)

target_compile_definitions(ignition_bridge_objs PUBLIC -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)
target_include_directories(ignition_bridge_objs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${IGNITION_ROOT}/externals/openvr ${IGNITION_ROOT}/externals/nlohmann)

if (HAS_WINEBUILD)
    # Winebuild looks at the binary symbols. LTO breaks that.
    target_compile_options(ignition_bridge_objs PRIVATE -fno-lto)

    find_library(WINECRT0_LIBRARY NAMES winecrt0 PATHS /usr/lib/wine/x86_64-unix /usr/lib64/wine /usr/lib/wine /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine/x86_64-unix)
    find_library(WINE_KERNEL32_LIBRARY NAMES kernel32 PATHS /usr/lib/wine/x86_64-unix /usr/lib64/wine /usr/lib/wine /usr/lib/x86_64-linux-gnu/wine /usr/lib/x86_64-linux-gnu/wine/x86_64-unix)

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ignition_bridge.s
        COMMAND ${WINEBUILD_PATH} -m64 -fPIC --dll -E ${CMAKE_CURRENT_SOURCE_DIR}/ignition_bridge.spec -o ${CMAKE_CURRENT_BINARY_DIR}/ignition_bridge.s -- $<TARGET_OBJECTS:ignition_bridge_objs> ${WINECRT0_LIBRARY} ${WINE_KERNEL32_LIBRARY}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ignition_bridge.spec ignition_bridge_objs
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/ignition_bridge.s PROPERTIES LANGUAGE C)

    add_library(ignition_bridge SHARED
        $<TARGET_OBJECTS:ignition_bridge_objs>
        ${CMAKE_CURRENT_BINARY_DIR}/ignition_bridge.s
    )
    target_link_libraries(ignition_bridge PRIVATE ${WINECRT0_LIBRARY} ${WINE_KERNEL32_LIBRARY})
    target_link_options(ignition_bridge INTERFACE -Wl,--allow-shlib-undefined)

    add_custom_command(
        TARGET ignition_bridge POST_BUILD
        COMMAND ${WINEBUILD_PATH} --fixup-ctors $<TARGET_FILE:ignition_bridge>
        VERBATIM
    )
else()
    add_library(ignition_bridge SHARED $<TARGET_OBJECTS:ignition_bridge_objs>)
endif()

add_library(ignition_bridge_static STATIC $<TARGET_OBJECTS:ignition_bridge_objs>)

if (NOT WIN32)
    target_link_libraries(ignition_bridge PRIVATE ${CMAKE_DL_LIBS})
    target_link_libraries(ignition_bridge_static PRIVATE ${CMAKE_DL_LIBS})
endif()