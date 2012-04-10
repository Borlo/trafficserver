/*
  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <ts/ts.h>
#include <ts/remap.h>

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <unistd.h>

static void *
LuaAllocate(void * ud, void * ptr, size_t osize, size_t nsize)
{
  TSReleaseAssert(ud == NULL);

  if (nsize == 0) {
    TSfree(ptr);
    return NULL;
  }

  return TSrealloc(ptr, nsize);
}

static int
TSLuaDebug(lua_State * lua)
{
  const char * tag = luaL_checkstring(lua, 1);
  const char * message = luaL_checkstring(lua, 2);

  TSDebug(tag, "%s", message);
  return 0;
}

static const luaL_Reg LUAEXPORTS[] =
{
  { "debug", TSLuaDebug },
  { NULL, NULL}
};

static TSReturnCode
LuaPluginInit(lua_State * lua)
{
  TSReturnCode status = TS_ERROR;

  lua_getglobal(lua, "init");
  if (lua_isnil(lua, -1)) {
    // No "init" callback.
    return TS_SUCCESS;
  }

  if (lua_pcall(lua, 0, 1, 0) != 0) {
    TSDebug("lua", "init failed: %s", lua_tostring(lua, -1));
    lua_pop(lua, 1);
  }

  // Return type is bool; check it and pop it.
  if (lua_isboolean(lua, 1) && lua_isboolean(lua, 1)) {
    status = TS_SUCCESS;
  }

  lua_pop(lua, 1);
  return status;
}

static TSReturnCode
LuaPluginRelease(lua_State * lua)
{
  lua_getglobal(lua, "release");
  if (lua_isnil(lua, -1)) {
    // No "release" callback.
    return TS_SUCCESS;
  }

  if (lua_pcall(lua, 0, 0, 0) != 0) {
    TSDebug("lua", "release failed: %s", lua_tostring(lua, -1));
    lua_pop(lua, 1);
  }

  lua_close(lua);
  return TS_SUCCESS;
}

static TSRemapStatus
LuaPluginRemap(lua_State * lua, TSHttpTxn txn, TSRemapRequestInfo * rri)
{
  lua_Integer status = TSREMAP_ERROR;

  lua_getglobal(lua, "remap");
  if (lua_isnil(lua, -1)) {
    // No "remap" callback, better continue.
    return TSREMAP_NO_REMAP;
  }

  if (lua_pcall(lua, 0, 1, 0) != 0) {
    TSDebug("lua", "remap failed: %s", lua_tostring(lua, -1));
    lua_pop(lua, 1);
    return TSREMAP_ERROR;
  }

  // Return type is integer. It must be one of the REMAP constants.
  if (!lua_isnumber(lua, 1)) {
    lua_pop(lua, 1);
    return TSREMAP_ERROR;
  }

  status = lua_tointeger(lua, 1);
  lua_pop(lua, 1);

  // Lua remap plugins only get to say whether to continue the remap chain or to stop.
  switch (status) {
  case TSREMAP_DID_REMAP:
  case TSREMAP_NO_REMAP_STOP:
    return (TSRemapStatus)status;
  case TSREMAP_NO_REMAP:
  case TSREMAP_DID_REMAP_STOP:
  case TSREMAP_ERROR:
  default:
    return TSREMAP_ERROR;
  }
}

TSReturnCode
TSRemapInit(TSRemapInterface * api_info, char * errbuf, int errbuf_size)
{
  TSDebug("lua", "loading lua plugin");
  return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char * argv[], void ** ih, char * errbuf, int errbuf_size)
{
  lua_State * lua;

  lua = lua_newstate(LuaAllocate, NULL);
  if (lua == NULL) {
    return TS_ERROR;
  }

  luaL_openlibs(lua);

  // Register functions in the "ts" module.
  luaL_register(lua, "ts", LUAEXPORTS);

  // Get the "ts" module table back on the stack.
  lua_getglobal(lua, "ts");
  TSReleaseAssert(lua_istable(lua, -1));

  // Push constants into the "ts" module.
  lua_pushinteger(lua, TSREMAP_DID_REMAP_STOP);
  lua_setfield(lua, -2, "REMAP_COMPLETE");

  lua_pushinteger(lua, TSREMAP_DID_REMAP);
  lua_setfield(lua, -2, "REMAP_CONTINUE");

  // Pop the "ts" module table.
  lua_pop(lua, 1);

  for (int i = 0; i < argc; ++i) {
    if (access(argv[i], R_OK) == 0) {
      TSDebug("lua", "%s loading lua file %s", __func__, argv[i]);
      if (luaL_dofile(lua, argv[i]) != 0) {
        // If the load failed, it should have pushed an error message.
        TSError("lua load error: %s", lua_tostring(lua, -1));
        lua_pop(lua, 1);
      }
    }
  }

  if (LuaPluginInit(lua) == TS_SUCCESS) {
    *ih = lua;
    return TS_SUCCESS;
  } else {
    lua_close(lua);
    return TS_ERROR;
  }
}

void
TSRemapDeleteInstance(void * ih)
{
  lua_State * lua = (lua_State *)ih;

  LuaPluginRelease(lua);
  lua_close(lua);
}

TSRemapStatus
TSRemapDoRemap(void * ih, TSHttpTxn txn, TSRemapRequestInfo * rri)
{
  return LuaPluginRemap((lua_State *)ih, txn, rri);
}