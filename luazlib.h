#ifndef LUAZLIB_H
#define LUAZLIB_H

struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif
int luaopen_luazlib(lua_State *L);
#ifdef __cplusplus
}
#endif
#endif // LUA_ZLIB.H
