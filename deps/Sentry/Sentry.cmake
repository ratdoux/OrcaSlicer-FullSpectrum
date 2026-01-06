set(_sentry_platform_flags
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DSENTRY_BUILD_TESTS=OFF
  -DSENTRY_EXAMPLES=OFF
  -DSENTRY_BACKEND=crashpad
  -DSENTRY_ENABLE_INSTALL=ON
)

# Platform-specific CMake generator and flags
set(_sentry_cmake_generator "")
set(_sentry_build_config "Release")

if (WIN32)
  # Windows: build shared libs so we get sentry.dll
  set(_sentry_platform_flags  ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_WINHTTP=ON
    -DSENTRY_BUILD_SHARED_LIBS=ON
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
    -DCMAKE_C_FLAGS_RELWITHDEBINFO:STRING=/Zi /O2
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=/Zi /O2
    -DCMAKE_EXE_LINKER_FLAGS:STRING=/DEBUG
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=/DEBUG
  )
  if (MSVC)
    set(_sentry_cmake_generator -G "Visual Studio 17 2022")
  endif()
elseif (APPLE)
  # macOS: build shared libs so we get libsentry.dylib
  # Note: CURL transport requires OpenSSL, need to link it explicitly
  # DESTDIR already contains /usr/local/ suffix, so use it directly
  set(_sentry_platform_flags 
    ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_CURL=ON
    -DSENTRY_BUILD_SHARED_LIBS=ON
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
    -DOPENSSL_ROOT_DIR:PATH=${DESTDIR}
    -DOPENSSL_USE_STATIC_LIBS:BOOL=ON
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=-L${DESTDIR}/lib\ -lssl\ -lcrypto
  )
  set(_sentry_cmake_generator -G "Unix Makefiles")
  
  # Sentry/crashpad requires macOS 12.0+ due to kIOMainPortDefault API usage
  # Force minimum deployment target to 12.0 for Sentry build
  set(_sentry_osx_deployment_target "12.0")
  if (CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER "12.0")
    set(_sentry_osx_deployment_target ${CMAKE_OSX_DEPLOYMENT_TARGET})
  endif()
  
  # Add macOS architecture and deployment target for sentry build
  if (CMAKE_OSX_ARCHITECTURES)
    set(_sentry_platform_flags ${_sentry_platform_flags} -DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES})
  endif()
  set(_sentry_platform_flags ${_sentry_platform_flags} -DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${_sentry_osx_deployment_target})
  if (CMAKE_OSX_SYSROOT)
    set(_sentry_platform_flags ${_sentry_platform_flags} -DCMAKE_OSX_SYSROOT:STRING=${CMAKE_OSX_SYSROOT})
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Linux: Use Unix Makefiles
  set(_sentry_platform_flags 
    ${_sentry_platform_flags}
    -DSENTRY_TRANSPORT_CURL=ON
    -DSENTRY_BUILD_SHARED_LIBS=OFF
    -DCMAKE_BUILD_TYPE:STRING=RelWithDebInfo
  )
  set(_sentry_cmake_generator -G "Unix Makefiles")
endif ()

if(WIN32)
  set(SENTRY_PATCH_COMMAND 
     ${GIT_EXECUTABLE} submodule update --init --recursive && ${CMAKE_COMMAND} -S external/crashpad -B external/crashpad/build ${_sentry_cmake_generator} -DCMAKE_BUILD_TYPE=Release && ${CMAKE_COMMAND} --build external/crashpad/build --config Release
  )
elseif(APPLE)
  set(SENTRY_PATCH_COMMAND 
     ${GIT_EXECUTABLE} submodule update --init --recursive
  )
else()
  set(SENTRY_PATCH_COMMAND 
     ${GIT_EXECUTABLE} submodule update --init --recursive
  )
endif()

Snapmaker_Orca_add_cmake_project(Sentry
  GIT_REPOSITORY      https://github.com/getsentry/sentry-native.git
  GIT_TAG             0.12.2
  GIT_SHALLOW         ON
  PATCH_COMMAND       ${SENTRY_PATCH_COMMAND}
  CMAKE_ARGS
    ${_sentry_cmake_generator}
    -DCMAKE_INSTALL_DATADIR:STRING=share
    ${_sentry_platform_flags}
)

# Sentry depends on CURL which depends on OpenSSL
# Ensure they are built before Sentry
if(APPLE)
	if (TARGET dep_CURL)
		add_dependencies(dep_Sentry dep_CURL)
	endif()
	if (TARGET dep_OpenSSL)
		add_dependencies(dep_Sentry dep_OpenSSL)
	endif()
endif()

if (MSVC)
    add_debug_dep(dep_Sentry)
endif ()
