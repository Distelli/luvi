# - find libyaml
# YAML_INCLUDE_DIR - Where to find header files (directory)
# YAML_LIBRARIES - Location of libraries
# YAML_LIBRARY_RELEASE - Where the release library is
# YAML_LIBRARY_DEBUG - Where the debug library is
# YAML_FOUND - Set to TRUE if we found everything (library, includes and executable)

IF( YAML_INCLUDE_DIR AND YAML_LIBRARY_RELEASE AND YAML_LIBRARY_DEBUG )
    SET(YAML_FIND_QUIETLY TRUE)
ENDIF( YAML_INCLUDE_DIR AND YAML_LIBRARY_RELEASE AND YAML_LIBRARY_DEBUG )

FIND_PATH( YAML_INCLUDE_DIR yaml.h  )

FIND_LIBRARY(YAML_LIBRARY_RELEASE NAMES yaml )

FIND_LIBRARY(YAML_LIBRARY_DEBUG NAMES yaml yamld  HINTS /usr/lib/debug/usr/lib/ )

IF( YAML_LIBRARY_RELEASE OR YAML_LIBRARY_DEBUG AND YAML_INCLUDE_DIR )
	SET( YAML_FOUND TRUE )
ENDIF( YAML_LIBRARY_RELEASE OR YAML_LIBRARY_DEBUG AND YAML_INCLUDE_DIR )

IF( YAML_LIBRARY_DEBUG AND YAML_LIBRARY_RELEASE )
	# if the generator supports configuration types then set
	# optimized and debug libraries, or if the CMAKE_BUILD_TYPE has a value
	IF( CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE )
		SET( YAML_LIBRARIES optimized ${YAML_LIBRARY_RELEASE} debug ${YAML_LIBRARY_DEBUG} )
	ELSE( CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE )
    # if there are no configuration types and CMAKE_BUILD_TYPE has no value
    # then just use the release libraries
		SET( YAML_LIBRARIES ${YAML_LIBRARY_RELEASE} )
	ENDIF( CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE )
ELSEIF( YAML_LIBRARY_RELEASE )
	SET( YAML_LIBRARIES ${YAML_LIBRARY_RELEASE} )
ELSE( YAML_LIBRARY_DEBUG AND YAML_LIBRARY_RELEASE )
	SET( YAML_LIBRARIES ${YAML_LIBRARY_DEBUG} )
ENDIF( YAML_LIBRARY_DEBUG AND YAML_LIBRARY_RELEASE )

IF( YAML_FOUND )
	IF( NOT YAML_FIND_QUIETLY )
		MESSAGE( STATUS "Found yaml header file in ${YAML_INCLUDE_DIR}")
		MESSAGE( STATUS "Found yaml libraries: ${YAML_LIBRARIES}")
	ENDIF( NOT YAML_FIND_QUIETLY )
ELSE(YAML_FOUND)
	IF( YAML_FIND_REQUIRED)
		MESSAGE( FATAL_ERROR "Could not find libyaml" )
	ELSE( YAML_FIND_REQUIRED)
		MESSAGE( STATUS "Optional package libyaml was not found" )
	ENDIF( YAML_FIND_REQUIRED)
ENDIF(YAML_FOUND)
