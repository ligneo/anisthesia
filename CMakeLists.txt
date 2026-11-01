cmake_minimum_required(VERSION 3.20)

project(anisthesia
	VERSION 1.0.0
	DESCRIPTION "Media detection library"
	HOMEPAGE_URL https://github.com/erengy/anisthesia
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(anisthesia INTERFACE)

target_include_directories(anisthesia INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)

target_sources(anisthesia INTERFACE
	src/matroska.cpp
	src/platform.cpp
	src/player.cpp
	src/util.cpp
)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	target_sources(anisthesia INTERFACE
		src/win_open_files.cpp
		src/win_platform.cpp
		src/win_strategies.cpp
		src/win_ui_automation.cpp
		src/win_util.cpp
		src/win_windows.cpp
	)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_sources(anisthesia INTERFACE
		src/lin_mpris.cpp
		src/lin_open_files.cpp
		src/lin_platform.cpp
		src/lin_processes.cpp
		src/lin_strategies.cpp
	)

	# Media control (MPRIS) needs sd-bus, which is provided by systemd, elogind
	# or basu. It can be left out with -DANISTHESIA_MPRIS=OFF, in which case
	# players are detected by their open files only and web browsers are not
	# detected at all.
	option(ANISTHESIA_MPRIS "Read media control information via sd-bus" ON)
	# With "auto" the first provider found in this order is used. A system with
	# more than one can pin it, e.g. -DANISTHESIA_SD_BUS_PROVIDER=libelogind.
	set(sd_bus_providers libsystemd libelogind basu)
	set(ANISTHESIA_SD_BUS_PROVIDER auto CACHE STRING "Provider of sd-bus: auto, ${sd_bus_providers}")
	set_property(CACHE ANISTHESIA_SD_BUS_PROVIDER PROPERTY STRINGS auto ${sd_bus_providers})
	if (ANISTHESIA_MPRIS)
		if (ANISTHESIA_SD_BUS_PROVIDER STREQUAL "auto")
			set(sd_bus_candidates ${sd_bus_providers})
		elseif (ANISTHESIA_SD_BUS_PROVIDER IN_LIST sd_bus_providers)
			set(sd_bus_candidates ${ANISTHESIA_SD_BUS_PROVIDER})
		else()
			message(FATAL_ERROR "ANISTHESIA_SD_BUS_PROVIDER must be auto or one of: ${sd_bus_providers}")
		endif()
		# Each provider is looked up under its own prefix, so that a cached
		# result for one does not stand in for another when the choice changes.
		find_package(PkgConfig REQUIRED)
		unset(sd_bus_provider)
		foreach (provider IN LISTS sd_bus_candidates)
			pkg_check_modules(SDBUS_${provider} QUIET IMPORTED_TARGET GLOBAL ${provider})
			if (SDBUS_${provider}_FOUND)
				set(sd_bus_provider ${provider})
				break()
			endif()
		endforeach()
		if (NOT DEFINED sd_bus_provider)
			string(REPLACE ";" ", " sd_bus_list "${sd_bus_candidates}")
			message(FATAL_ERROR "MPRIS needs sd-bus from ${sd_bus_list}. "
				"Install it, or configure with -DANISTHESIA_MPRIS=OFF.")
		endif()
		message(STATUS "anisthesia: sd-bus from ${sd_bus_provider}")
		target_compile_definitions(anisthesia INTERFACE ANISTHESIA_MPRIS)
		# basu installs its headers under basu/ instead of systemd/
		if (sd_bus_provider STREQUAL "basu")
			target_compile_definitions(anisthesia INTERFACE ANISTHESIA_MPRIS_BASU)
		endif()
		target_link_libraries(anisthesia INTERFACE PkgConfig::SDBUS_${sd_bus_provider})
	endif()
endif()
