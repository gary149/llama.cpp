set(AGENT_CORE_TARGET llama-agent-core)

set(AGENT_CORE_SOURCES
    agent-loop.cpp
    agent-loop-compaction.cpp
    agent-loop-completion.cpp
    agent-loop-tools.cpp
    agent-resources.cpp
    agent-tool-parser.cpp
    clipboard-image.cpp
    compaction.cpp
    local-inference-backend.cpp
    session-file.cpp
    tool-registry.cpp
    permission-policy.cpp
    permission.cpp
    permission-async.cpp
    skills/skills-manager.cpp
    agents-md/agents-md-manager.cpp
    tools/tool-bash.cpp
    tools/tool-read.cpp
    tools/tool-write.cpp
    tools/tool-edit.cpp
    tools/tool-glob.cpp
    tools/tool-plan.cpp
)

# MCP support only available on Unix (uses fork, pipe, poll)
if(NOT WIN32)
    list(APPEND AGENT_CORE_SOURCES
        mcp/mcp-client.cpp
        mcp/mcp-server-manager.cpp
        mcp/mcp-tool-wrapper.cpp
    )
endif()

if(LLAMA_HTTPLIB)
    list(APPEND AGENT_CORE_SOURCES
        http-inference-backend.cpp
    )
endif()

add_library(${AGENT_CORE_TARGET} STATIC ${AGENT_CORE_SOURCES})
target_link_libraries(${AGENT_CORE_TARGET} PUBLIC server-context llama-common ${CMAKE_THREAD_LIBS_INIT})
if(LLAMA_HTTPLIB)
    target_link_libraries(${AGENT_CORE_TARGET} PUBLIC cpp-httplib)
    target_compile_definitions(${AGENT_CORE_TARGET} PUBLIC LLAMA_AGENT_HAS_HTTP_BACKEND=1)
endif()
target_compile_features(${AGENT_CORE_TARGET} PUBLIC cxx_std_17)
target_include_directories(${AGENT_CORE_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../server
    ${CMAKE_SOURCE_DIR}/vendor
)

# Force the whole agent-core archive into a consumer so the REGISTER_TOOL static
# initializers (tools self-register at startup) survive linking. Without this the
# linker drops the unreferenced tool translation units from the static library and
# the agent starts with an empty tool registry (it can describe actions but never
# call a tool). Uses $<TARGET_FILE> rather than the target name so the archive path
# stays adjacent to the --whole-archive flags (CMake reorders bare target names).
function(agent_force_link_core tgt)
    if(MSVC)
        target_link_libraries(${tgt} PRIVATE ${AGENT_CORE_TARGET})
        target_link_options(${tgt} PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:${AGENT_CORE_TARGET}>")
    elseif(APPLE)
        target_link_libraries(${tgt} PRIVATE
            -Wl,-force_load,$<TARGET_FILE:${AGENT_CORE_TARGET}> ${AGENT_CORE_TARGET})
    else()
        target_link_libraries(${tgt} PRIVATE
            -Wl,--whole-archive $<TARGET_FILE:${AGENT_CORE_TARGET}> -Wl,--no-whole-archive
            ${AGENT_CORE_TARGET})
    endif()
endfunction()

set(TARGET llama-agent)

add_executable(${TARGET}
    agent.cpp
    agent-loop-run.cpp
    agent-loop-completion-cli.cpp
    agent-loop-tools-cli.cpp
    permission-prompt.cpp
    stb-image-impl.cpp
    tui-editor.cpp
    tui-renderer.cpp
    tui-select-list.cpp
)
agent_force_link_core(${TARGET})
target_compile_features(${TARGET} PRIVATE cxx_std_17)
# stb-image-impl.cpp compiles the vendored stb_image.h, which trips -Wcast-qual under
# LLAMA_FATAL_WARNINGS; suppress it for this target only (same as tools/mtmd does).
if (NOT MSVC)
    target_compile_options(${TARGET} PRIVATE -Wno-cast-qual)
endif()

include_directories(../server)
include_directories(${CMAKE_SOURCE_DIR}/vendor)  # for stb/stb_image.h (terminal image preview)

if(LLAMA_TOOLS_INSTALL)
    install(TARGETS ${TARGET} RUNTIME)
endif()

# llama-agent-server (HTTP API for agent)
if(LLAMA_HTTPLIB)
    set(AGENT_SERVER_TARGET llama-agent-server)

    set(AGENT_SERVER_SOURCES
        server/agent-server.cpp
        server/agent-session.cpp
        server/agent-routes.cpp
        ../server/server-http.cpp
    )

    add_executable(${AGENT_SERVER_TARGET} ${AGENT_SERVER_SOURCES})
    target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE server-context llama-ui PUBLIC llama-common cpp-httplib ${CMAKE_THREAD_LIBS_INIT})
    # Whole-archive agent-core so the tool REGISTER_TOOL static initializers link (see helper above).
    agent_force_link_core(${AGENT_SERVER_TARGET})
    target_compile_features(${AGENT_SERVER_TARGET} PRIVATE cxx_std_17)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ../server)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
    # Include directory for generated server assets (index.html.gz.hpp, loading.html.hpp)
    target_include_directories(${AGENT_SERVER_TARGET} PRIVATE ${CMAKE_BINARY_DIR}/tools/server)
    # Depend on llama-server to ensure generated assets exist
    add_dependencies(${AGENT_SERVER_TARGET} llama-server)

    if(WIN32)
        target_link_libraries(${AGENT_SERVER_TARGET} PRIVATE ws2_32)
    endif()

    if(LLAMA_TOOLS_INSTALL)
        install(TARGETS ${AGENT_SERVER_TARGET} RUNTIME)
    endif()
endif()

# Agent unit tests
if(LLAMA_BUILD_TESTS)
    add_executable(test-agent-compaction test-compaction.cpp compaction.cpp)
    target_link_libraries(test-agent-compaction PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-compaction PRIVATE cxx_std_17)
    target_include_directories(test-agent-compaction PRIVATE ../server)
    add_test(NAME agent-compaction COMMAND test-agent-compaction)

    add_executable(test-agent-session-file test-session-file.cpp session-file.cpp)
    target_link_libraries(test-agent-session-file PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-session-file PRIVATE cxx_std_17)
    target_include_directories(test-agent-session-file PRIVATE ../server)
    add_test(NAME agent-session-file COMMAND test-agent-session-file)

    add_executable(test-agent-resources
        test-resources.cpp
        agent-resources.cpp
        skills/skills-manager.cpp
        agents-md/agents-md-manager.cpp
    )
    target_link_libraries(test-agent-resources PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-resources PRIVATE cxx_std_17)
    target_include_directories(test-agent-resources PRIVATE ../server)
    add_test(NAME agent-resources COMMAND test-agent-resources)

    add_executable(test-agent-permission
        test-permission.cpp
        permission-policy.cpp
        permission.cpp
        permission-async.cpp
    )
    target_link_libraries(test-agent-permission PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-permission PRIVATE cxx_std_17)
    target_include_directories(test-agent-permission PRIVATE ../server)
    add_test(NAME agent-permission COMMAND test-agent-permission)

    add_executable(test-agent-tool-parser
        test-tool-parser.cpp
        agent-tool-parser.cpp
    )
    target_link_libraries(test-agent-tool-parser PRIVATE llama-common ${CMAKE_THREAD_LIBS_INIT})
    target_compile_features(test-agent-tool-parser PRIVATE cxx_std_17)
    target_include_directories(test-agent-tool-parser PRIVATE ../server)
    add_test(NAME agent-tool-parser COMMAND test-agent-tool-parser)

    add_executable(test-agent-inference-backend
        test-inference-backend.cpp
        agent-loop-run.cpp
        agent-loop-completion-cli.cpp
        agent-loop-tools-cli.cpp
        permission-prompt.cpp
        stb-image-impl.cpp
    )
    target_link_libraries(test-agent-inference-backend PRIVATE ${AGENT_CORE_TARGET})
    target_compile_features(test-agent-inference-backend PRIVATE cxx_std_17)
    target_include_directories(test-agent-inference-backend PRIVATE ../server)
    if (NOT MSVC)
        target_compile_options(test-agent-inference-backend PRIVATE -Wno-cast-qual)  # vendored stb_image.h
    endif()
    add_test(NAME agent-inference-backend COMMAND test-agent-inference-backend)

    add_executable(test-agent-tui
        test-tui.cpp
        tui-editor.cpp
        tui-renderer.cpp
        tui-select-list.cpp
    )
    target_link_libraries(test-agent-tui PRIVATE ${AGENT_CORE_TARGET})
    target_compile_features(test-agent-tui PRIVATE cxx_std_17)
    target_include_directories(test-agent-tui PRIVATE ../server)
    add_test(NAME agent-tui COMMAND test-agent-tui)

    option(LLAMA_AGENT_TERMINAL_TESTS "Run agent PTY rendering tests (requires Python and pyte)" OFF)
    if(LLAMA_AGENT_TERMINAL_TESTS)
        if(WIN32)
            message(FATAL_ERROR "Agent PTY tests currently require a POSIX host")
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        execute_process(COMMAND ${Python3_EXECUTABLE} -c "import pyte" RESULT_VARIABLE PYTE_STATUS)
        if(NOT PYTE_STATUS EQUAL 0)
            message(FATAL_ERROR "Install tools/agent/tests/requirements.txt into the selected Python environment")
        endif()
        add_test(NAME agent-terminal COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_terminal.py
            --executable $<TARGET_FILE:test-agent-tui>
            --artifacts ${CMAKE_CURRENT_BINARY_DIR}/terminal-artifacts/plain)
        add_test(NAME agent-terminal-color COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_terminal.py --color
            --executable $<TARGET_FILE:test-agent-tui>
            --artifacts ${CMAKE_CURRENT_BINARY_DIR}/terminal-artifacts/color)
        set_tests_properties(agent-terminal PROPERTIES TIMEOUT 600)
        set_tests_properties(agent-terminal-color PROPERTIES TIMEOUT 600)
    endif()

    if(LLAMA_HTTPLIB)
        add_executable(test-agent-http-inference-backend
            test-http-inference-backend.cpp
        )
        target_link_libraries(test-agent-http-inference-backend PRIVATE ${AGENT_CORE_TARGET} cpp-httplib)
        target_compile_features(test-agent-http-inference-backend PRIVATE cxx_std_17)
        target_include_directories(test-agent-http-inference-backend PRIVATE ../server)
        add_test(NAME agent-http-inference-backend COMMAND test-agent-http-inference-backend)
    endif()
endif()
