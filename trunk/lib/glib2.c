/* Copyright (C) 2007, 2008 MySQL AB */ 

#include <glib.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static int lua_g_usleep (lua_State *L) {
	int ms = luaL_checkinteger (L, 1);

	g_usleep(ms);

	return 0;
}

static int lua_g_checksum_md5 (lua_State *L) {
	size_t str_len;
	const char *str = luaL_checklstring (L, 1, &str_len);
	GChecksum *cs;

	cs = g_checksum_new(G_CHECKSUM_MD5);

	g_checksum_update(cs, (guchar *)str, str_len);

	lua_pushstring(L, g_checksum_get_string(cs));

	g_checksum_free(cs);

	return 1;
}

/*
** Assumes the table is on top of the stack.
*/
static void set_info (lua_State *L) {
	lua_pushliteral (L, "_COPYRIGHT");
	lua_pushliteral (L, "Copyright (C) 2007, 2008 MySQL AB");
	lua_settable (L, -3);
	lua_pushliteral (L, "_DESCRIPTION");
	lua_pushliteral (L, "export glib2-functions as glib.*");
	lua_settable (L, -3);
	lua_pushliteral (L, "_VERSION");
	lua_pushliteral (L, "LuaGlib2 0.1");
	lua_settable (L, -3);
}


static const struct luaL_reg gliblib[] = {
	{"usleep", lua_g_usleep},
	{"md5", lua_g_checksum_md5},
	{NULL, NULL},
};

int luaopen_glib2 (lua_State *L) {
	luaL_openlib (L, "glib2", gliblib, 0);
	set_info (L);
	return 1;
}