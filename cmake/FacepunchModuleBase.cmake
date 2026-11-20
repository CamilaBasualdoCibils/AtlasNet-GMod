include(FetchContent)
FetchContent_Declare(gmod_module_base
  URL https://codeload.github.com/Facepunch/gmod-module-base/tar.gz/4fdafe7d762fe63f0360c67d9e119a129292712e
  DOWNLOAD_NAME gmod-module-base.tar.gz
  URL_HASH SHA256=4c829d898fa2314b7b0ccd09a19baf53210fe8ff3204a86901748a1904099ae7)
FetchContent_MakeAvailable(gmod_module_base)

# Adapt the official headers in the build tree for the x86-64 branch.
# The layout and virtuals through SetState match garrysmod_common's
# GarrysMod/Lua/Interface.h and LuaBase.h.
set(facepunch_include "${CMAKE_CURRENT_BINARY_DIR}/facepunch-include")
set(lua_header_dir "${gmod_module_base_SOURCE_DIR}/include/GarrysMod/Lua")
set(lua_output_dir "${facepunch_include}/GarrysMod/Lua")
configure_file("${lua_header_dir}/Types.h" "${lua_output_dir}/Types.h" COPYONLY)
configure_file("${lua_header_dir}/UserData.h" "${lua_output_dir}/UserData.h" COPYONLY)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${lua_header_dir}/Interface.h" "${lua_header_dir}/LuaBase.h")
file(READ "${lua_header_dir}/Interface.h" interface_header)
string(REPLACE "_ignore_this_common_lua_header_[69]"
  "_ignore_this_common_lua_header_[114]" interface_x64 "${interface_header}")
file(READ "${lua_header_dir}/LuaBase.h" lua_base_header)
string(REPLACE "struct lua_State;" "struct lua_State;\nclass QAngle;\nclass Vector;"
  lua_base_x64 "${lua_base_header}")
set(last_virtual "CheckNumber( int iStackPos = -1 ) = 0;")
string(REPLACE "${last_virtual}" "${last_virtual}
                virtual int ObjLen( int iStackPos = -1 ) = 0;
                virtual const QAngle& GetAngle( int iStackPos = -1 ) = 0;
                virtual const Vector& GetVector( int iStackPos = -1 ) = 0;
                virtual void PushAngle( const QAngle& val ) = 0;
                virtual void PushVector( const Vector& val ) = 0;
                virtual void SetState( lua_State* L ) = 0;"
  lua_base_with_state "${lua_base_x64}")
if(interface_header STREQUAL interface_x64 OR lua_base_x64 STREQUAL lua_base_with_state)
  message(FATAL_ERROR "The pinned Facepunch headers no longer match the Linux x86-64 adaptation")
endif()
file(CONFIGURE OUTPUT "${lua_output_dir}/Interface.h" CONTENT "${interface_x64}" @ONLY)
file(CONFIGURE OUTPUT "${lua_output_dir}/LuaBase.h" CONTENT "${lua_base_with_state}" @ONLY)
