include(FindPkgConfig)
PKG_CHECK_MODULES(libxmlpp REQUIRED libxml++-2.6)

include_directories(${libxmlpp_INCLUDE_DIRS})
link_directories(${libxmlpp_LIBRARY_DIRS})

IF (libxmlpp_INCLUDE_DIRS) 
	MESSAGE("Libxml++ libraries found at ${libxmlpp_LIBRARY_DIRS}")
	list(APPEND DEPS libxml++2.6-2)
	list(APPEND DEPS_DEV libxml++2.6-dev)
ELSE()
	MESSAGE ("*** Libxml++ not found")
ENDIF()



