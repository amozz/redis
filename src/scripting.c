#include "redis.h"
#include "sha1.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

void scriptingInit(void) {
    lua_State *lua = lua_open();
    luaL_openlibs(lua);
    server.lua = lua;
}

/* Hash the scripit into a SHA1 digest. We use this as Lua function name.
 * Digest should point to a 41 bytes buffer: 40 for SHA1 converted into an
 * hexadecimal number, plus 1 byte for null term. */
void hashScript(char *digest, char *script, size_t len) {
    SHA1_CTX ctx;
    unsigned char hash[20];
    char *cset = "0123456789abcdef";
    int j;

    SHA1Init(&ctx);
    SHA1Update(&ctx,(unsigned char*)script,len);
    SHA1Final(hash,&ctx);

    for (j = 0; j < 20; j++) {
        digest[j*2] = cset[((hash[j]&0xF0)>>4)];
        digest[j*2+1] = cset[(hash[j]&0xF)];
    }
    digest[40] = '\0';
}

void luaReplyToRedisReply(redisClient *c, lua_State *lua) {
    int t = lua_type(lua,1);

    switch(t) {
    case LUA_TSTRING:
        addReplyBulkCBuffer(c,(char*)lua_tostring(lua,1),lua_strlen(lua,1));
        break;
    case LUA_TBOOLEAN:
        addReply(c,lua_toboolean(lua,1) ? shared.cone : shared.czero);
        break;
    case LUA_TNUMBER:
        addReplyLongLong(c,(long long)lua_tonumber(lua,1));
        break;
    default:
        addReply(c,shared.nullbulk);
    }
    lua_pop(lua,1);
}

void evalCommand(redisClient *c) {
    lua_State *lua = server.lua;
    char funcname[43];

    /* We obtain the script SHA1, then check if this function is already
     * defined into the Lua state */
    funcname[0] = 'f';
    funcname[1] = '_';
    hashScript(funcname+2,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
    lua_getglobal(lua, funcname);
    if (lua_isnil(lua,1)) {
        /* Function not defined... let's define it. */
        sds funcdef = sdsempty();

        lua_pop(lua,1); /* remove the nil from the stack */
        funcdef = sdscat(funcdef,"function ");
        funcdef = sdscatlen(funcdef,funcname,42);
        funcdef = sdscatlen(funcdef," ()\n",4);
        funcdef = sdscatlen(funcdef,c->argv[1]->ptr,sdslen(c->argv[1]->ptr));
        funcdef = sdscatlen(funcdef,"\nend\n",5);
        printf("Defining:\n%s\n",funcdef);

        if (luaL_loadbuffer(lua,funcdef,sdslen(funcdef),"func definition")) {
            addReplyErrorFormat(c,"Error compiling script (new function): %s\n",
                lua_tostring(lua,-1));
            lua_pop(lua,1);
            return;
        }
        if (lua_pcall(lua,0,0,0)) {
            addReplyErrorFormat(c,"Error running script (new function): %s\n",
                lua_tostring(lua,-1));
            lua_pop(lua,1);
            return;
        }
        lua_getglobal(lua, funcname);
    }
    
    /* At this point whatever this script was never seen before or if it was
     * already defined, we can call it. We have zero arguments and expect
     * a single return value. */
    if (lua_pcall(lua,0,1,0)) {
        addReplyErrorFormat(c,"Error running script (call to %s): %s\n",
            funcname, lua_tostring(lua,-1));
        lua_pop(lua,1);
        return;
    }
    luaReplyToRedisReply(c,lua);
}