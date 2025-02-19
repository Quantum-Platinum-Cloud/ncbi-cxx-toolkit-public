#############################################################################
# $Id$
#############################################################################
#############################################################################
##
##  NCBI CMake wrapper
##
##  Summary


get_directory_property(_CompileDefs COMPILE_DEFINITIONS)
list(SORT _CompileDefs)
get_directory_property(_CompileOptions COMPILE_OPTIONS)
string(REPLACE ";" " " _CompileOptions "${_CompileOptions}")
get_directory_property(_LinkOptions LINK_OPTIONS)
string(REPLACE ";" " " _LinkOptions "${_LinkOptions}")

if ( NOT "${NCBI_MODULES_FOUND}" STREQUAL "")
    list(REMOVE_DUPLICATES NCBI_MODULES_FOUND)
endif()
foreach (mod ${NCBI_MODULES_FOUND})
    set(MOD_STR "${MOD_STR} ${mod}")
endforeach()

#STRING(SUBSTRING "${EXTERNAL_LIBRARIES_COMMENT}" 1 -1 EXTERNAL_LIBRARIES_COMMENT)

message("")
if(DEFINED NCBI_SIGNATURE)
    if (WIN32 OR XCODE)
        message("NCBI_SIGNATURE:        ${NCBI_SIGNATURE_CFG}")
    else()
        message("NCBI_SIGNATURE:        ${NCBI_SIGNATURE}")
    endif()
endif()
if(DEFINED NCBITEST_SIGNATURE)
    message("NCBITEST_SIGNATURE:    ${NCBITEST_SIGNATURE}")
endif()
if($ENV{NCBI_AUTOMATED_BUILD})
    message("NCBI_AUTOMATED_BUILD:  $ENV{NCBI_AUTOMATED_BUILD}")
endif()
if($ENV{NCBI_CHECK_DB_LOAD})
    message("NCBI_CHECK_DB_LOAD:    $ENV{NCBI_CHECK_DB_LOAD}")
endif()
if($ENV{NCBIPTB_INSTALL_CHECK})
    message("NCBIPTB_INSTALL_CHECK: $ENV{NCBIPTB_INSTALL_CHECK}")
endif()
if($ENV{NCBIPTB_INSTALL_SRC})
    message("NCBIPTB_INSTALL_SRC:   $ENV{NCBIPTB_INSTALL_SRC}")
endif()
message("------------------------------------------------------------------------------")
message("CMake exe:      ${CMAKE_COMMAND}")
message("CMake version:  ${CMAKE_VERSION}")
message("Compiler:       ${NCBI_COMPILER} v${NCBI_COMPILER_VERSION}")
message("Build Type:     ${NCBI_CONFIGURATION_TYPES}")
message("Shared Libs:    ${BUILD_SHARED_LIBS}")
message("Top Source Dir: ${NCBI_SRC_ROOT}")
message("Build Root:     ${CMAKE_BINARY_DIR}")
message("Executable Dir: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message("Archive Dir:    ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}")
message("Library Dir:    ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
message("C Compiler:     ${CMAKE_C_COMPILER}")
message("C++ Compiler:   ${CMAKE_CXX_COMPILER}")
if (CMAKE_USE_DISTCC AND DISTCC_EXECUTABLE)
    message("    distcc:     ${DISTCC_EXECUTABLE}")
endif()
if (CMAKE_USE_CCACHE AND CCACHE_EXECUTABLE)
    message("    ccache:     ${CCACHE_EXECUTABLE}")
endif()
set(_BuildType)
if("${NCBI_CONFIGURATION_TYPES_COUNT}" EQUAL 1)
    string(TOUPPER "${NCBI_CONFIGURATION_TYPES}" _BuildType)
endif()
message("CFLAGS:        ${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${_BuildType}} ${_CompileOptions}")
if(NOT "${NCBI_CONFIGURATION_TYPES_COUNT}" EQUAL 1)
        foreach(_cfg IN LISTS NCBI_CONFIGURATION_TYPES)
            string(TOUPPER "${_cfg}" _ucfg)
            message("       ${_cfg}:  ${CMAKE_C_FLAGS_${_ucfg}}")
        endforeach()
endif()
message("CXXFLAGS:      ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_BuildType}} ${_CompileOptions}")
if(NOT "${NCBI_CONFIGURATION_TYPES_COUNT}" EQUAL 1)
        foreach(_cfg IN LISTS NCBI_CONFIGURATION_TYPES)
            string(TOUPPER "${_cfg}" _ucfg)
            message("       ${_cfg}:  ${CMAKE_CXX_FLAGS_${_ucfg}}")
        endforeach()
endif()
message("EXE_LINKER_FLAGS:    ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${_BuildType}} ${_LinkOptions}")
if(NOT "${NCBI_CONFIGURATION_TYPES_COUNT}" EQUAL 1)
        foreach(_cfg IN LISTS NCBI_CONFIGURATION_TYPES)
            string(TOUPPER "${_cfg}" _ucfg)
            message("       ${_cfg}:  ${CMAKE_EXE_LINKER_FLAGS_${_ucfg}}")
        endforeach()
endif()
if (BUILD_SHARED_LIBS)
    message("SHARED_LINKER_FLAGS:  ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS_${_BuildType}} ${_LinkOptions}")
    if(NOT "${NCBI_CONFIGURATION_TYPES_COUNT}" EQUAL 1)
            foreach(_cfg IN LISTS NCBI_CONFIGURATION_TYPES)
                string(TOUPPER "${_cfg}" _ucfg)
                message("       ${_cfg}:  ${CMAKE_SHARED_LINKER_FLAGS_${_ucfg}}")
            endforeach()
    endif()
endif()
if (OFF)
    message("STATIC_LINKER_FLAGS: ${CMAKE_STATIC_LINKER_FLAGS} ${CMAKE_STATIC_LINKER_FLAGS_${_BuildType}}")
    if (APPLE)
        message("MODULE_LINKER_FLAGS:  ${CMAKE_MODULE_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS_${_BuildType}} ${_LinkOptions}")
    endif()
endif()
message("Compile Definitions: ${_CompileDefs}")
message("DataTool Ver:   ${_datatool_version}")
message("DataTool Path:  ${NCBI_DATATOOL}")
message("")
message("Components:  ${NCBI_ALL_COMPONENTS}")
message("Requirements:  ${NCBI_ALL_REQUIRES}")
message("Disabled Components:  ${NCBI_ALL_DISABLED}")
message("Deprecated Components:  ${NCBI_ALL_LEGACY}")
message("Compile Features:  ${NCBI_PTBCFG_PROJECT_FEATURES}")

message("------------------------------------------------------------------------------")
message("")
