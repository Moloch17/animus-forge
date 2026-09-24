# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# The curriculum, the environment, the encoders and the bots all live in src/ now. They were animus-lib, a library
# shared with mod-animus and carried here as a git subtree; mod-animus keeps its own copy (and with it the model
# runtime that plays exported models), and this module no longer has a dependency to resolve, a bundle to refresh, or
# a second loader to call.
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
