#include <ctype.h>
#define LUA_LIB
#include <lauxlib.h>
#include <lua.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define DEF_MEM_LEVEL 8

typedef uLong(*checksum_t)        (uLong crc, const Bytef *buf, uInt len);
typedef uLong(*checksum_combine_t)(uLong crc1, uLong crc2, z_off_t len2);

static int lualz_deflate(lua_State *L);
static int lualz_deflate_delete(lua_State *L);
static int lualz_inflate_delete(lua_State *L);
static int lualz_inflate(lua_State *L);
static int lualz_checksum(lua_State *L);
static int lualz_checksum_new(lua_State *L, checksum_t checksum, checksum_combine_t combine);
static int lualz_adler32(lua_State *L);
static int lualz_crc32(lua_State *L);

static int lualz_version(lua_State *L) {
	const char* version = zlibVersion();
	int         count = strlen(version) + 1;
	char*       cur = (char*)memcpy(lua_newuserdata(L, count),
		version, count);
	count = 0;
	while (*cur) {
		char* begin = cur;
		while (isdigit(*cur)) cur++;
		if (begin != cur) {
			int is_end = *cur == '\0';
			*cur = '\0';
			lua_pushnumber(L, atoi(begin));
			count++;
			if (is_end) break;
			cur++;
		}
		while (*cur && !isdigit(*cur)) cur++;
	}
	return count;
}

static int lualz_assert(lua_State *L, int result, const z_stream* stream, const char* file, int line) {
	if (result == Z_OK || result == Z_STREAM_END) return result;
	switch (result) {
	case Z_NEED_DICT:
		lua_pushfstring(L, "RequiresDictionary: input stream requires a dictionary to be deflated (%s) at %s line %d",
			stream->msg, file, line);
		break;
	case Z_STREAM_ERROR:
		lua_pushfstring(L, "InternalError: inconsistent internal zlib stream (%s) at %s line %d",
			stream->msg, file, line);
		break;
	case Z_DATA_ERROR:
		lua_pushfstring(L, "InvalidInput: input string does not conform to zlib format or checksum failed at %s line %d",
			file, line);
		break;
	case Z_MEM_ERROR:
		lua_pushfstring(L, "OutOfMemory: not enough memory (%s) at %s line %d",
			stream->msg, file, line);
		break;
	case Z_BUF_ERROR:
		lua_pushfstring(L, "InternalError: no progress possible (%s) at %s line %d",
			stream->msg, file, line);
		break;
	case Z_VERSION_ERROR:
		lua_pushfstring(L, "IncompatibleLibrary: built with version %s, but dynamically linked with version %s (%s) at %s line %d",
			ZLIB_VERSION, zlibVersion(), stream->msg, file, line);
		break;
	default:
		lua_pushfstring(L, "ZLibError: unknown code %d (%s) at %s line %d",
			result, stream->msg, file, line);
	}
	lua_error(L);
	return result;
}

static int lualz_filter_impl(lua_State *L, int(*filter)(z_streamp, int), int(*end)(z_streamp), char* name) {
	int flush = Z_NO_FLUSH, result;
	z_stream* stream;
	luaL_Buffer buff;
	size_t avail_in;

	if (filter == deflate) {
		const char *const opts[] = { "none", "sync", "full", "finish", NULL };
		flush = luaL_checkoption(L, 2, opts[0], opts);
		if (flush) flush++;
		if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
			flush = Z_FINISH;
		}
	}

	stream = (z_stream*)lua_touserdata(L, lua_upvalueindex(1));
	if (stream == NULL) {
		if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
			lua_pushfstring(L, "IllegalState: calling %s function when stream was previously closed", name);
			lua_error(L);
		}
		lua_pushstring(L, "");
		lua_pushboolean(L, 1);
		return 2; /* Ignore duplicate calls to "close". */
	}

	luaL_buffinit(L, &buff);

	if (lua_gettop(L) > 1) lua_pushvalue(L, 1);

	if (lua_isstring(L, lua_upvalueindex(2))) {
		lua_pushvalue(L, lua_upvalueindex(2));
		if (lua_gettop(L) > 1 && lua_isstring(L, -2)) {
			lua_concat(L, 2);
		}
	}
	if (lua_gettop(L) > 0) {
		stream->next_in = (unsigned char*)lua_tolstring(L, -1, &avail_in);
	}
	else {
		stream->next_in = NULL;
		avail_in = 0;
	}
	stream->avail_in = avail_in;

	if (!stream->avail_in && !flush) {
		lua_pushstring(L, "");
		lua_pushboolean(L, 0);
		lua_pushinteger(L, stream->total_in);
		lua_pushinteger(L, stream->total_out);
		return 4;
	}

	do {
		stream->next_out = (unsigned char*)luaL_prepbuffer(&buff);
		stream->avail_out = LUAL_BUFFERSIZE;
		result = filter(stream, flush);
		if (Z_BUF_ERROR != result) {
			lualz_assert(L, result, stream, __FILE__, __LINE__);
		}
		luaL_addsize(&buff, LUAL_BUFFERSIZE - stream->avail_out);
	} while (stream->avail_out == 0);

	luaL_pushresult(&buff);

	if (NULL != stream->next_in) {
		lua_pushlstring(L, (char*)stream->next_in, stream->avail_in);
		lua_replace(L, lua_upvalueindex(2));
	}

	if (result == Z_STREAM_END) {
		lua_pushnil(L);
		lua_setmetatable(L, lua_upvalueindex(1));
		lua_pushnil(L);
		lua_replace(L, lua_upvalueindex(1));
		lualz_assert(L, end(stream), stream, __FILE__, __LINE__);
		lua_pushboolean(L, 1);
	}
	else {
		lua_pushboolean(L, 0);
	}
	lua_pushinteger(L, stream->total_in);
	lua_pushinteger(L, stream->total_out);
	return 4;
}

static void lualz_create_deflate_mt(lua_State *L) {
	luaL_newmetatable(L, "lz.deflate.meta"); /*  {} */

	lua_pushcfunction(L, lualz_deflate_delete);
	lua_setfield(L, -2, "__gc");

	lua_pop(L, 1); /*  <empty> */
}

static int lualz_deflate_new(lua_State *L) {
	int level;
	int window_size;
	int result;

	level = luaL_optint(L, 1, Z_DEFAULT_COMPRESSION);
	window_size = luaL_optint(L, 2, MAX_WBITS);
	z_stream* stream = (z_stream*)lua_newuserdata(L, sizeof(z_stream));
	stream->zalloc = Z_NULL;
	stream->zfree = Z_NULL;
	result = deflateInit2(stream, level, Z_DEFLATED, window_size,
		DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);

	lualz_assert(L, result, stream, __FILE__, __LINE__);
	luaL_getmetatable(L, "lz.deflate.meta");
	lua_setmetatable(L, -2);
	lua_pushnil(L);
	lua_pushcclosure(L, lualz_deflate, 2);
	return 1;
}

static int lualz_deflate(lua_State *L) {
	return lualz_filter_impl(L, deflate, deflateEnd, "deflate");
}

static int lualz_deflate_delete(lua_State *L) {
	z_stream* stream = (z_stream*)lua_touserdata(L, 1);

	deflateEnd(stream);

	return 0;
}


static void lualz_create_inflate_mt(lua_State *L) {
	luaL_newmetatable(L, "lz.inflate.meta"); /*  {} */

	lua_pushcfunction(L, lualz_inflate_delete);
	lua_setfield(L, -2, "__gc");

	lua_pop(L, 1); /*  <empty> */
}

static int lualz_inflate_new(lua_State *L) {
	z_stream* stream;
	stream = (z_stream*)lua_newuserdata(L, sizeof(z_stream));

	int window_size = lua_isnumber(L, 1) ? lua_tointeger(L, 1) : MAX_WBITS + 32;
	stream->zalloc = Z_NULL;
	stream->zfree = Z_NULL;
	stream->next_in = Z_NULL;
	stream->avail_in = 0;
	lualz_assert(L, inflateInit2(stream, window_size), stream, __FILE__, __LINE__);

	luaL_getmetatable(L, "lz.inflate.meta");
	lua_setmetatable(L, -2);

	lua_pushnil(L);
	lua_pushcclosure(L, lualz_inflate, 2);
	return 1;
}

static int lualz_inflate(lua_State *L) {
	return lualz_filter_impl(L, inflate, inflateEnd, "inflate");
}

static int lualz_inflate_delete(lua_State *L) {
	z_stream* stream = (z_stream*)lua_touserdata(L, 1);
	inflateEnd(stream);
	return 0;
}

static int lualz_checksum(lua_State *L) {
	if (lua_gettop(L) <= 0) {
		lua_pushvalue(L, lua_upvalueindex(3));
		lua_pushvalue(L, lua_upvalueindex(4));
	}
	else if (lua_isfunction(L, 1)) {
		checksum_combine_t combine = (checksum_combine_t)
			lua_touserdata(L, lua_upvalueindex(2));

		lua_pushvalue(L, 1);
		lua_call(L, 0, 2);
		if (!lua_isnumber(L, -2) || !lua_isnumber(L, -1)) {
			luaL_argerror(L, 1, "expected function to return two numbers");
		}
		lua_pushnumber(L,
			combine((uLong)lua_tonumber(L, lua_upvalueindex(3)),
			(uLong)lua_tonumber(L, -2),
				(z_off_t)lua_tonumber(L, -1)));
		lua_pushvalue(L, -1);
		lua_replace(L, lua_upvalueindex(3));
		lua_pushnumber(L,
			lua_tonumber(L, lua_upvalueindex(4)) + lua_tonumber(L, -2));
		lua_pushvalue(L, -1);
		lua_replace(L, lua_upvalueindex(4));
	}
	else {
		const Bytef* str;
		size_t       len;

		checksum_t checksum = (checksum_t)
			lua_touserdata(L, lua_upvalueindex(1));
		str = (const Bytef*)luaL_checklstring(L, 1, &len);

		lua_pushnumber(L,
			checksum((uLong)lua_tonumber(L, lua_upvalueindex(3)),
				str,
				len));
		lua_pushvalue(L, -1);
		lua_replace(L, lua_upvalueindex(3));

		lua_pushnumber(L,
			lua_tonumber(L, lua_upvalueindex(4)) + len);
		lua_pushvalue(L, -1);
		lua_replace(L, lua_upvalueindex(4));
	}
	return 2;
}

static int lualz_checksum_new(lua_State *L, checksum_t checksum, checksum_combine_t combine) {
	lua_pushlightuserdata(L, checksum);
	lua_pushlightuserdata(L, combine);
	lua_pushnumber(L, checksum(0L, Z_NULL, 0));
	lua_pushnumber(L, 0);
	lua_pushcclosure(L, lualz_checksum, 4);
	return 1;
}

static int lualz_adler32(lua_State *L) {
	return lualz_checksum_new(L, adler32, adler32_combine);
}

static int lualz_crc32(lua_State *L) {
	return lualz_checksum_new(L, crc32, crc32_combine);
}


static const luaL_Reg luazlib_functions[] = {
	{ "deflate", lualz_deflate_new },
	{ "inflate", lualz_inflate_new },
	{ "adler32", lualz_adler32     },
	{ "crc32",   lualz_crc32       },
	{ "version", lualz_version     },
	{ NULL,      NULL           }
};

#define SETLITERAL(n,v) (lua_pushliteral(L, n), lua_pushliteral(L, v), lua_settable(L, -3))
#define SETINT(n,v) (lua_pushliteral(L, n), lua_pushinteger(L, v), lua_settable(L, -3))

int luaopen_luazlib(lua_State *L) {
	lualz_create_deflate_mt(L);
	lualz_create_inflate_mt(L);
	luaL_register(L, "zlib", luazlib_functions);
	SETINT("BEST_SPEED", Z_BEST_SPEED);
	SETINT("BEST_COMPRESSION", Z_BEST_COMPRESSION);
	SETINT("_TEST_BUFSIZ", LUAL_BUFFERSIZE);
	return 1;
}
