# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# mod-animus-forge needs animus-lib, the code it shares with mod-animus. Its source is bundled in animus-lib/ (a git
# subtree of https://github.com/Moloch17/animus-lib; tools/update-animus-lib.sh updates it), so this folder builds
# offline. A modules/mod-animus-lib checkout, when present, is built instead of the bundle.
#
# Installing also creates etc/modules/mod_animus_forge.conf from its .dist when there is none: AzerothCore reads a
# module's settings from the .conf only, and without one every AnimusForge key logs "Missing property" and keeps its
# default. An existing .conf is never overwritten.

ModuleNameToVariable(mod-animus-forge ANIMUS_FORGE_LINKAGE_VARIABLE)
if(NOT "${${ANIMUS_FORGE_LINKAGE_VARIABLE}}" MATCHES "static|dynamic")
  return()
endif()

# Where the core installs module configs (CopyModuleConfig, src/cmake/macros/ConfigInstall.cmake).
if(WIN32)
  set(ANIMUS_MODULE_CONF_DIR "${CMAKE_INSTALL_PREFIX}/configs/modules")
else()
  set(ANIMUS_MODULE_CONF_DIR "${CONF_DIR}/modules")
endif()
install(CODE "
  set(animusConf \"\$ENV{DESTDIR}${ANIMUS_MODULE_CONF_DIR}/mod_animus_forge.conf\")
  if(NOT EXISTS \"\${animusConf}\")
    message(STATUS \"Creating: \${animusConf}\")
    configure_file(\"${CMAKE_CURRENT_LIST_DIR}/conf/mod_animus_forge.conf.dist\" \"\${animusConf}\" COPYONLY)
  endif()")

set(ANIMUS_LIB_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/animus-lib")
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
  include("${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
else()
  include("${ANIMUS_LIB_BUNDLE}/cmake/AnimusLibDependency.cmake")
endif()
AnimusLibRequire(mod-animus-forge "${ANIMUS_LIB_BUNDLE}")
