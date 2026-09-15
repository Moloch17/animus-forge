# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# mod-animus-forge needs animus-lib, the code it shares with mod-animus. When modules/mod-animus-lib is missing it is
# cloned from ANIMUS_LIB_GIT_URL at ANIMUS_LIB_GIT_REF and built with this configure.

set(ANIMUS_LIB_GIT_URL "https://github.com/Moloch17/animus-lib.git" CACHE STRING
  "Where mod-animus and mod-animus-forge clone animus-lib from when modules/mod-animus-lib is missing")
set(ANIMUS_LIB_GIT_REF "master" CACHE STRING "The animus-lib branch or tag to clone")

ModuleNameToVariable(mod-animus-forge ANIMUS_FORGE_LINKAGE_VARIABLE)
if(NOT "${${ANIMUS_FORGE_LINKAGE_VARIABLE}}" MATCHES "static|dynamic")
  return()
endif()

set(ANIMUS_LIB_CHECKOUT "${CMAKE_SOURCE_DIR}/modules/mod-animus-lib")
if(NOT EXISTS "${ANIMUS_LIB_CHECKOUT}/cmake/AnimusLibDependency.cmake")
  if(EXISTS "${ANIMUS_LIB_CHECKOUT}")
    message(FATAL_ERROR "${ANIMUS_LIB_CHECKOUT} exists but is not animus-lib; remove it to have it cloned again")
  endif()

  find_package(Git REQUIRED)
  message(STATUS "  mod-animus-forge: cloning animus-lib ${ANIMUS_LIB_GIT_REF} from ${ANIMUS_LIB_GIT_URL}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" clone --branch "${ANIMUS_LIB_GIT_REF}" "${ANIMUS_LIB_GIT_URL}" "${ANIMUS_LIB_CHECKOUT}"
    RESULT_VARIABLE ANIMUS_LIB_CLONE_RESULT)
  if(NOT ANIMUS_LIB_CLONE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not clone animus-lib from ${ANIMUS_LIB_GIT_URL}; clone it into ${ANIMUS_LIB_CHECKOUT} "
      "by hand")
  endif()
endif()

include("${ANIMUS_LIB_CHECKOUT}/cmake/AnimusLibDependency.cmake")
AnimusLibRequire(mod-animus-forge)
