// NukeScript — a gameplay plugin that adds a Lua-backed ScriptComponent, registered into
// the engine's shared reflection registry at load.
//
// Script convention: the .lua file returns { props = {...}, update = function(self, dt) end }.
// `props` is shared BY REFERENCE with the inspector (edits are seen live) and persisted in
// the hidden serialized `props` JSON field.

#include <interface/NUKEEInteface.h>   // NUKEModule + AppInstance
#include <interface/AssetCreators.h>   // register a "New Lua Script" browser command (data only)
#include <interface/IconsFileTypes.h>    // ICON_FT_*: the glyph vocabulary a file type can claim
#include <interface/iGUI.h>            // runtime GUI facade (scripts' gui() draws via this)
#include <service/iScript.h>           // the scripting SERVICE contract this plugin provides
#include <reflect/Reflect.h>
#include <reflect/ReflectBind.h>       // reflection->script layer: generic component access (0.8)
#include <API/Model/Atom.h>
#include <API/Model/Transform.h>
#include <API/Model/Time.h>
#include <API/Model/World.h>           // World::Settings (fixedUpdate dt) + game lock
#include <API/Model/Audio.h>           // sound content: PlayData blob channel

#include <thread>
#include <chrono>
#include <boost/filesystem.hpp>
#include <set>
#include <API/Model/Package.h>   // packed sessions: scripts live in mounted paks
#include <lua.hpp>
#include <LuaBridge/LuaBridge.h>
#include <nlohmann/json.hpp>          // persist edited prop values

#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace std;
using namespace nuke;
namespace bfs = boost::filesystem;
namespace lb = luabridge;
using json = nlohmann::json;

// The one Lua VM, created lazily on the thread that first runs a script — never the loader thread.
static lua_State* gL = nullptr;

// Reflection-driven component proxy: a userdata {atomId, componentId} resolved through
// ReflectBind. Stale-safe — reads on a dead handle yield nil, writes raise a Lua error.
// Values map as Vec3<->nuke.Vector3, Vec2/Vec4/Quat<->{x,y,z,w}, Color<->{r,g,b,a}.
struct CompRef { unsigned long atomId; unsigned long compId; };
static const char* kCompMeta = "nuke.Component";

// Reflected OBJECT handles (defined below): forward pieces the component proxy uses to
// expose asset-reference fields as objects rather than guids.
struct ObjRef { unsigned long id; };
static const char* kObjMeta = "nuke.Object";
static int PushObjHandle(lua_State* L, unsigned long id);
static const Field* AssetAliasField(const TypeInfo* ti, const std::string& key);

static Component* CompRefResolve(lua_State* L, int idx)
{
    CompRef* r = (CompRef*)luaL_checkudata(L, idx, kCompMeta);
    return Reflect_ResolveComponent(r->atomId, r->compId);
}

// Push a reflected value as the matching Lua value (see mapping above).
static int PushReflectValue(lua_State* L, const ReflectValue& v)
{
    switch (v.type)
    {
        case FT::Bool:   lua_pushboolean(L, v.b); break;
        case FT::Int:
        case FT::Float:
        case FT::Double: lua_pushnumber(L, v.num); break;
        case FT::String: lua_pushlstring(L, v.str.data(), v.str.size()); break;
        case FT::Vec3:
            if (!lb::push(L, Vector3(v.v[0], v.v[1], v.v[2])))
                lua_pushnil(L);
            break;
        case FT::Vec2:
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "y");
            break;
        case FT::Vec4:
        case FT::Quat:
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "y");
            lua_pushnumber(L, v.v[2]); lua_setfield(L, -2, "z");
            lua_pushnumber(L, v.v[3]); lua_setfield(L, -2, "w");
            break;
        case FT::Color:
            lua_createtable(L, 0, 4);
            lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "r");
            lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "g");
            lua_pushnumber(L, v.v[2]); lua_setfield(L, -2, "b");
            lua_pushnumber(L, v.v[3]); lua_setfield(L, -2, "a");
            break;
        case FT::AtomRef:
        {
            Atom* a = Reflect_AtomById(v.atom);   // stale-safe: dead id -> nil
            if (!a || !lb::push(L, a)) lua_pushnil(L);
            break;
        }
        case FT::ObjectRef:                       // reflected instance -> nuke.Object handle
            PushObjHandle(L, v.obj);              // 0 -> nil
            break;
        default: lua_pushnil(L); break;
    }
    return 1;
}

// Read one numeric slot of a vector-ish table ("x"/"y"/... or "r"/"g"/...); keeps the
// previous value when the key is absent (so partial tables like {x=1} work).
static void ReadTableSlot(lua_State* L, int idx, const char* key, double& into)
{
    lua_getfield(L, idx, key);
    if (lua_isnumber(L, -1)) into = lua_tonumber(L, -1);
    lua_pop(L, 1);
}

// Convert the Lua value at `idx` into a ReflectValue of field type `ft`.
// `cur` = the field's current value (partial table writes keep unset slots).
static bool ReadReflectValue(lua_State* L, int idx, FT ft, const ReflectValue& cur, ReflectValue& out)
{
    out = cur;
    out.type = ft;
    switch (ft)
    {
        case FT::Bool:
            out.b = lua_toboolean(L, idx) != 0;
            return true;
        case FT::Int:
        case FT::Float:
        case FT::Double:
            if (!lua_isnumber(L, idx)) return false;
            out.num = lua_tonumber(L, idx);
            return true;
        case FT::String:
        {
            // An object handle where a string is expected = an asset reference: its guid.
            if (ObjRef* o = (ObjRef*)luaL_testudata(L, idx, kObjMeta))
            {
                out.str = Reflect_ObjectGuid(o->id);
                return true;
            }
            const char* s = lua_tostring(L, idx);
            if (!s) return false;
            out.str = s;
            return true;
        }
        case FT::Vec3:
            if (lua_isuserdata(L, idx))   // a bound nuke.Vector3
            {
                lb::LuaRef ref = lb::LuaRef::fromStack(L, idx);
                if (ref.isInstance<Vector3>())
                {
                    auto tv = ref.cast<Vector3>();
                    if (!tv) return false;
                    out.v[0] = tv->x; out.v[1] = tv->y; out.v[2] = tv->z;
                    return true;
                }
                return false;
            }
            [[fallthrough]];
        case FT::Vec2:
        case FT::Vec4:
        case FT::Quat:
            if (!lua_istable(L, idx)) return false;
            ReadTableSlot(L, idx, "x", out.v[0]);
            ReadTableSlot(L, idx, "y", out.v[1]);
            ReadTableSlot(L, idx, "z", out.v[2]);
            ReadTableSlot(L, idx, "w", out.v[3]);
            return true;
        case FT::Color:
            if (!lua_istable(L, idx)) return false;
            ReadTableSlot(L, idx, "r", out.v[0]);
            ReadTableSlot(L, idx, "g", out.v[1]);
            ReadTableSlot(L, idx, "b", out.v[2]);
            ReadTableSlot(L, idx, "a", out.v[3]);
            return true;
        case FT::AtomRef:
        {
            if (lua_isnil(L, idx)) { out.atom = 0; return true; }
            lb::LuaRef ref = lb::LuaRef::fromStack(L, idx);
            if (!ref.isInstance<Atom>()) return false;
            auto ta = ref.cast<Atom*>();
            if (!ta) return false;
            out.atom = Reflect_AtomId(*ta);
            return true;
        }
        case FT::ObjectRef:
        {
            if (lua_isnil(L, idx)) { out.obj = 0; return true; }   // null object
            ObjRef* r = (ObjRef*)luaL_testudata(L, idx, kObjMeta);
            if (!r) return false;
            out.obj = r->id;
            return true;
        }
        default:
            return false;
    }
}

// Bound method call `comp:Method(args...)`. Upvalues are (atomId, compId, methodName) —
// ids and a name, never pointers, so a stale call errors instead of crashing.
static int CompMethodCall(lua_State* L)
{
    unsigned long atomId = (unsigned long)lua_tointeger(L, lua_upvalueindex(1));
    unsigned long compId = (unsigned long)lua_tointeger(L, lua_upvalueindex(2));
    const char*   mname  = lua_tostring(L, lua_upvalueindex(3));
    Component* c = Reflect_ResolveComponent(atomId, compId);
    if (!c) return luaL_error(L, "nuke.Component: component no longer exists (stale handle)");
    const Method* m = Reflect_FindMethod(c->GetType(), mname);
    if (!m) return luaL_error(L, "nuke.Component: method '%s' vanished (plugin toggled?)", mname);

    // Colon call puts the handle at slot 1 and args after it; a dot-call starts args at 1.
    const int base = luaL_testudata(L, 1, kCompMeta) ? 2 : 1;
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "nuke.Component: %s expects %d argument(s)", mname, (int)nargs);

    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];   // no "current value" for an argument
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.Component: bad argument #%d to %s", (int)i + 1, mname);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(c, *m, args.data(), nargs, ret))
        return luaL_error(L, "nuke.Component: invoking %s failed", mname);
    if (ret.type == FT::Unknown) return 0;               // void
    return PushReflectValue(L, ret);
}

static int CompIndex(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (strcmp(key, "valid") == 0) { lua_pushboolean(L, c != nullptr); return 1; }
    if (!c) { lua_pushnil(L); return 1; }               // dead handle: reads yield nil
    if (strcmp(key, "enabled") == 0) { lua_pushboolean(L, c->enabled); return 1; }
    if (strcmp(key, "tickEvery") == 0) { lua_pushinteger(L, c->tickEvery); return 1; }   // tick interval (6.8)
    if (strcmp(key, "type") == 0)
    {
        lua_pushstring(L, c->GetType() ? c->GetType()->name.c_str() : c->name);
        return 1;
    }
    if (strcmp(key, "atom") == 0)
    {
        if (!c->atom || !lb::push(L, c->atom)) lua_pushnil(L);
        return 1;
    }
    const Field* f = Reflect_FindField(c->GetType(), key);
    if (f && (f->type == FT::IntList || f->type == FT::FloatList || f->type == FT::DoubleList || f->type == FT::StringList))
    {
        // LIST prop -> a plain Lua array table, via the JSON bridge (ReflectValue holds no vectors).
        nlohmann::json j = nlohmann::json::parse(Reflect_GetFieldJson(c, *f), nullptr, false);
        lua_newtable(L);
        if (j.is_array())
        {
            int i = 1;
            for (auto& e : j)
            {
                if (e.is_string()) lua_pushstring(L, e.get<std::string>().c_str());
                else               lua_pushnumber(L, e.is_number() ? e.get<double>() : 0.0);
                lua_rawseti(L, -2, i++);
            }
        }
        return 1;
    }
    if (f) return PushReflectValue(L, Reflect_GetField(c, *f));
    if (Reflect_FindMethod(c->GetType(), key))           // [[nuke::func]] method -> bound closure
    {
        CompRef* r = (CompRef*)lua_touserdata(L, 1);
        lua_pushinteger(L, (lua_Integer)r->atomId);
        lua_pushinteger(L, (lua_Integer)r->compId);
        lua_pushstring(L, key);
        lua_pushcclosure(L, CompMethodCall, 3);
        return 1;
    }
    // OBJECT view of an asset-reference field: `mr.mesh` (field meshGuid) -> object handle.
    if (const Field* af = AssetAliasField(c->GetType(), key))
        return PushObjHandle(L, Reflect_ObjectFromGuid(Reflect_GetField(c, *af).str));
    // A component-OWNED object: `mr.material` -> its live material instance.
    {
        CompRef* r = (CompRef*)lua_touserdata(L, 1);
        if (unsigned long oid = Reflect_ComponentObject(r->atomId, r->compId, key))
            return PushObjHandle(L, oid);
    }
    lua_pushnil(L);                                      // unknown name: Lua-idiomatic nil
    return 1;
}

static int CompNewIndex(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (!c) return luaL_error(L, "nuke.Component: component no longer exists (stale handle)");
    if (strcmp(key, "enabled") == 0) { c->enabled = lua_toboolean(L, 3) != 0; return 0; }
    if (strcmp(key, "tickEvery") == 0) { int v = (int)lua_tointeger(L, 3); c->tickEvery = v < 1 ? 1 : v; return 0; }   // (6.8)
    const Field* f = Reflect_FindField(c->GetType(), key);
    if (!f) f = AssetAliasField(c->GetType(), key);      // `mr.mesh = obj` writes meshGuid
    if (!f)
        return luaL_error(L, "nuke.Component: '%s' has no property '%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    // LIST prop: a Lua array table replaces the whole vector, via the serializer's JSON encoding.
    if (f->type == FT::IntList || f->type == FT::FloatList || f->type == FT::DoubleList || f->type == FT::StringList)
    {
        if (!lua_istable(L, 3))
            return luaL_error(L, "nuke.Component: '%s.%s' is a list — assign an array table",
                              c->GetType() ? c->GetType()->name.c_str() : c->name, key);
        nlohmann::json j = nlohmann::json::array();
        const int n = (int)lua_rawlen(L, 3);
        for (int i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, 3, i);
            if (f->type == FT::StringList) { const char* s = lua_tostring(L, -1); j.push_back(s ? s : ""); }
            else                           j.push_back(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        if (!Reflect_SetFieldJson(c, *f, j.dump()))
            return luaL_error(L, "nuke.Component: bad list value for '%s.%s'",
                              c->GetType() ? c->GetType()->name.c_str() : c->name, key);
        Reflect_ComponentFieldChanged(c, *f);
        return 0;
    }
    ReflectValue v;
    if (!f->asset.empty() && lua_isnil(L, 3))            // nil clears an asset reference
    {
        v.type = FT::String;
        v.str.clear();
    }
    else if (!ReadReflectValue(L, 3, f->type, Reflect_GetField(c, *f), v))
        return luaL_error(L, "nuke.Component: bad value for '%s.%s'",
                          c->GetType() ? c->GetType()->name.c_str() : c->name, key);
    Reflect_SetField(c, *f, v);
    Reflect_ComponentFieldChanged(c, *f);                // asset writes take effect this frame
    return 0;
}

static int CompToString(lua_State* L)
{
    Component* c = CompRefResolve(L, 1);
    if (!c) { lua_pushstring(L, "nuke.Component(<dead>)"); return 1; }
    lua_pushfstring(L, "nuke.Component(%s)", c->GetType() ? c->GetType()->name.c_str() : c->name);
    return 1;
}

static void RegisterComponentProxy(lua_State* L)
{
    luaL_newmetatable(L, kCompMeta);
    lua_pushcfunction(L, CompIndex);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, CompNewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, CompToString); lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
}

// Wrap a live component into a stale-safe handle (ids, not pointers). Pass the owning
// `atom` explicitly: Component::atom may still be unset before Init.
static lb::LuaRef MakeComponentRef(lua_State* L, Atom* atom, Component* c)
{
    CompRef* r = (CompRef*)lua_newuserdata(L, sizeof(CompRef));
    r->atomId = atom->id.id;
    r->compId = c->id.id;
    luaL_setmetatable(L, kCompMeta);
    lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
    lua_pop(L, 1);
    return ref;
}

// Reflected OBJECT handles: a nuke.Object userdata carries only a ReflectBind ObjTable id
// (the same table C# uses). __index/__newindex dispatch fields and [[nuke::func]] methods
// through the registry. Builtins: valid, guid, type; Texture adds setPixels(w, h, rgba).
static int PushObjHandle(lua_State* L, unsigned long id)
{
    if (!id) { lua_pushnil(L); return 1; }
    ObjRef* r = (ObjRef*)lua_newuserdata(L, sizeof(ObjRef));
    r->id = id;
    luaL_setmetatable(L, kObjMeta);
    return 1;
}

// The OBJECT view of an asset-reference field: for a missing key `mesh`, matches the
// reflected `meshGuid` string field carrying asset metadata. Direct names always win.
static const Field* AssetAliasField(const TypeInfo* ti, const std::string& key)
{
    if (!ti || key.empty() || Reflect_FindField(ti, key)) return nullptr;
    const Field* f = Reflect_FindField(ti, key + "Guid");
    return (f && f->type == FT::String && !f->asset.empty()) ? f : nullptr;
}

static int ObjMethodInvoke(lua_State* L)   // upvalues: (objId, methodName)
{
    unsigned long id  = (unsigned long)lua_tointeger(L, lua_upvalueindex(1));
    const char*   mname = lua_tostring(L, lua_upvalueindex(2));
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(id));
    const Method* m = ti ? Reflect_FindMethod(ti, mname) : nullptr;
    if (!m) return luaL_error(L, "nuke.Object: method '%s' on a dead handle", mname);

    const int base = luaL_testudata(L, 1, kObjMeta) ? 2 : 1;   // colon call: self at slot 1
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "nuke.Object: %s expects %d argument(s)", mname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.Object: bad argument #%d to %s", (int)i + 1, mname);
    }
    ReflectValue ret;
    if (!Reflect_ObjectInvoke(id, mname, args.data(), nargs, ret))
        return luaL_error(L, "nuke.Object: invoking %s failed", mname);
    if (ret.type == FT::Unknown) return 0;
    return PushReflectValue(L, ret);
}

// Texture CONTENT: tex:setPixels(w, h, rgba) — rgba is a Lua string (the idiomatic byte
// buffer), tightly packed RGBA8, #rgba == w*h*4. Uploads live via the engine blob channel.
static int ObjSetPixels(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    size_t len = 0;
    const char* p = luaL_checklstring(L, 4, &len);
    lua_pushboolean(L, Reflect_SetTexturePixels(r->id, w, h, p, len));
    return 1;
}

// Read a flat Lua number array into `out`; false if the value is not a numeric table.
static bool ReadFloatArray(lua_State* L, int idx, std::vector<float>& out)
{
    if (!lua_istable(L, idx)) return false;
    size_t n = lua_rawlen(L, idx);
    out.resize(n);
    for (size_t i = 1; i <= n; ++i)
    {
        lua_rawgeti(L, idx, (lua_Integer)i);
        if (!lua_isnumber(L, -1)) { lua_pop(L, 1); return false; }
        out[i - 1] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

// mesh:setGeometry(verts [, normals [, uvs]]) — unindexed triangle list, verts = 9*T numbers,
// normals match verts (else computed flat), uvs 2 per vertex (else zeros).
static int ObjSetGeometry(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    std::vector<float> v, n, u;
    if (!ReadFloatArray(L, 2, v) || v.empty() || v.size() % 9 != 0)
        return luaL_error(L, "setGeometry: vertices must be a flat array of 9*T numbers (triangle list)");
    const bool hasN = lua_istable(L, 3);
    const bool hasU = lua_istable(L, 4);
    if (hasN && (!ReadFloatArray(L, 3, n) || n.size() != v.size()))
        return luaL_error(L, "setGeometry: normals must match vertices (3 numbers per vertex)");
    if (hasU && (!ReadFloatArray(L, 4, u) || u.size() != v.size() / 3 * 2))
        return luaL_error(L, "setGeometry: uvs must be 2 numbers per vertex");
    lua_pushboolean(L, Reflect_SetMeshGeometry(r->id, (int)(v.size() / 3), v.data(),
                                               hasN ? n.data() : nullptr,
                                               hasU ? u.data() : nullptr));
    return 1;
}

static int ObjIndex(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* key = luaL_checkstring(L, 2);
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(r->id));
    if (strcmp(key, "valid") == 0) { lua_pushboolean(L, ti != nullptr); return 1; }
    if (!ti) { lua_pushnil(L); return 1; }               // dead handle: reads yield nil
    if (strcmp(key, "guid") == 0)
    {
        std::string g = Reflect_ObjectGuid(r->id);
        lua_pushlstring(L, g.data(), g.size());
        return 1;
    }
    if (strcmp(key, "type") == 0) { lua_pushstring(L, ti->name.c_str()); return 1; }
    if (strcmp(key, "setPixels") == 0 && ti->name == "Texture")
    {
        lua_pushcfunction(L, ObjSetPixels);
        return 1;
    }
    if (strcmp(key, "setGeometry") == 0 && ti->name == "Mesh")
    {
        lua_pushcfunction(L, ObjSetGeometry);
        return 1;
    }
    if (Reflect_FindField(ti, key))
        return PushReflectValue(L, Reflect_ObjectGet(r->id, key));
    if (Reflect_FindMethod(ti, key))
    {
        lua_pushinteger(L, (lua_Integer)r->id);
        lua_pushstring(L, key);
        lua_pushcclosure(L, ObjMethodInvoke, 2);
        return 1;
    }
    if (const Field* af = AssetAliasField(ti, key))      // mat.shader / mat.diffuse -> objects
        return PushObjHandle(L, Reflect_ObjectFromGuid(Reflect_ObjectGet(r->id, af->name).str));
    lua_pushnil(L);
    return 1;
}

static int ObjNewIndex(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* key = luaL_checkstring(L, 2);
    TypeInfo* ti = Registry_Find(Reflect_ObjectType(r->id));
    if (!ti) return luaL_error(L, "nuke.Object: dead handle");
    const Field* f = Reflect_FindField(ti, key);
    if (!f) f = AssetAliasField(ti, key);                // mat.shader = obj writes shaderGuid
    if (!f) return luaL_error(L, "%s has no property '%s'", ti->name.c_str(), key);
    ReflectValue v;
    if (!f->asset.empty() && lua_isnil(L, 3))            // nil clears an asset reference
    {
        v.type = FT::String;
        v.str.clear();
    }
    else if (!ReadReflectValue(L, 3, f->type, Reflect_ObjectGet(r->id, f->name), v))
        return luaL_error(L, "%s.%s: bad value", ti->name.c_str(), key);
    if (!Reflect_ObjectSet(r->id, f->name, v))           // engine re-resolves asset refs
        return luaL_error(L, "%s.%s: write failed", ti->name.c_str(), key);
    return 0;
}

static int ObjToString(lua_State* L)
{
    ObjRef* r = (ObjRef*)luaL_checkudata(L, 1, kObjMeta);
    const char* t = Reflect_ObjectType(r->id);
    if (!*t) { lua_pushstring(L, "nuke.Object(<dead>)"); return 1; }
    lua_pushfstring(L, "nuke.Object(%s)", t);
    return 1;
}

static int ObjEq(lua_State* L)   // same engine handle = same object
{
    ObjRef* a = (ObjRef*)luaL_testudata(L, 1, kObjMeta);
    ObjRef* b = (ObjRef*)luaL_testudata(L, 2, kObjMeta);
    lua_pushboolean(L, a && b && a->id == b->id);
    return 1;
}

static void RegisterObjectProxy(lua_State* L)
{
    luaL_newmetatable(L, kObjMeta);
    lua_pushcfunction(L, ObjIndex);    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ObjNewIndex); lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, ObjToString); lua_setfield(L, -2, "__tostring");
    lua_pushcfunction(L, ObjEq);       lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);
}

// Per-type factories: nuke.<Type>.Create()/Find(name)/FromGuid(guid). Upvalue = type name.
static int TypeCreate(lua_State* L)
{
    return PushObjHandle(L, Reflect_CreateObject(lua_tostring(L, lua_upvalueindex(1))));
}
static int TypeFind(lua_State* L)
{
    return PushObjHandle(L, Reflect_FindAsset(lua_tostring(L, lua_upvalueindex(1)),
                                              luaL_checkstring(L, 1)));
}
static int TypeFromGuid(lua_State* L)
{
    const char* tname = lua_tostring(L, lua_upvalueindex(1));
    unsigned long id = Reflect_ObjectFromGuid(luaL_checkstring(L, 1));
    if (id && strcmp(Reflect_ObjectType(id), tname) != 0) id = 0;   // wrong class -> nil
    return PushObjHandle(L, id);
}

// Bound static reflected function nuke.<Type>.<Fn>(args...). Upvalues are (typeName, fnName)
// strings, resolved through the registry per call so re-registration can never dangle.
static int StaticFnCall(lua_State* L)
{
    const char* tname = lua_tostring(L, lua_upvalueindex(1));
    const char* fname = lua_tostring(L, lua_upvalueindex(2));
    TypeInfo* ti = Registry_Find(tname);
    const Method* m = ti ? Reflect_FindMethod(ti, fname) : nullptr;
    if (!m || !m->isStatic) return luaL_error(L, "nuke.%s.%s vanished (plugin toggled?)", tname, fname);

    const size_t nargs = m->params.size();
    if ((size_t)lua_gettop(L) < nargs)
        return luaL_error(L, "nuke.%s.%s expects %d argument(s)", tname, fname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, 1 + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "nuke.%s.%s: bad argument #%d", tname, fname, (int)i + 1);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(nullptr, *m, args.data(), nargs, ret))
        return luaL_error(L, "nuke.%s.%s: invoke failed", tname, fname);
    if (ret.type == FT::Unknown) return 0;   // void
    return PushReflectValue(L, ret);
}

// Expose every reflected static method as nuke.<Type>.<Fn>, plus the object factories
// Create()/Find(name)/FromGuid(guid) (components come from atom:addComponent instead).
static void BindReflectedStatics(lua_State* L)
{
    lua_getglobal(L, "nuke");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    for (TypeInfo* ti : Registry_All())
    {
        if (!ti) continue;
        // Create() for creatable non-components; Find/FromGuid only for ResDB assets (same rule as the C# generator).
        const bool creatable  = ti->create && ti->base != "Component";
        const bool assetLookup = Reflect_IsAssetType(ti->name);
        bool any = false;
        auto ensureTable = [&] { if (!any) { lua_newtable(L); any = true; } };
        for (const Method& m : ti->methods)
        {
            if (!m.isStatic) continue;
            ensureTable();
            lua_pushstring(L, m.name.c_str());        // key
            lua_pushstring(L, ti->name.c_str());      // upvalue 1: type name
            lua_pushstring(L, m.name.c_str());        // upvalue 2: fn name
            lua_pushcclosure(L, StaticFnCall, 2);     // value
            lua_rawset(L, -3);                        // subtable[fn] = closure
        }
        if (creatable || assetLookup)
        {
            ensureTable();
            struct { const char* name; lua_CFunction fn; bool on; } fns[] =
                { { "Create", TypeCreate, creatable },
                  { "Find", TypeFind, assetLookup },
                  { "FromGuid", TypeFromGuid, assetLookup } };
            for (auto& e : fns)
            {
                if (!e.on) continue;
                lua_pushstring(L, e.name);
                lua_pushstring(L, ti->name.c_str());  // upvalue: type name
                lua_pushcclosure(L, e.fn, 1);
                lua_rawset(L, -3);
            }
        }
        // nuke.Audio.PlayData(bytes [, volume [, loop [, bus]]]) — a blob, so hand-bound like setPixels.
        if (ti->name == "Audio")
        {
            ensureTable();
            lua_pushstring(L, "PlayData");
            lua_pushcfunction(L, +[](lua_State* LL) -> int {
                size_t len = 0;
                const char* p = luaL_checklstring(LL, 1, &len);
                double vol  = luaL_optnumber(LL, 2, 1.0);
                bool   loop = lua_toboolean(LL, 3) != 0;
                double bus  = luaL_optnumber(LL, 4, 1.0);
                lua_pushnumber(LL, Audio::PlayData(p, (uint64_t)len, vol, loop, bus));
                return 1;
            });
            lua_rawset(L, -3);
        }
        if (any)
        {
            // rawset: the LuaBridge namespace table is __newindex-protected (read-only).
            lua_pushstring(L, ti->name.c_str());
            lua_insert(L, -2);                        // key under the subtable
            lua_rawset(L, -3);                        // nuke[Type] = subtable
        }
    }
    // nuke.Assets.find(name [, type]) — any-class asset lookup by name; .fromGuid(g).
    lua_newtable(L);
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        const char* type = lua_isstring(LL, 2) ? lua_tostring(LL, 2) : "";
        return PushObjHandle(LL, Reflect_FindAsset(type, luaL_checkstring(LL, 1)));
    });
    lua_setfield(L, -2, "find");
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        return PushObjHandle(LL, Reflect_ObjectFromGuid(luaL_checkstring(LL, 1)));
    });
    lua_setfield(L, -2, "fromGuid");
    lua_pushstring(L, "Assets");
    lua_insert(L, -2);
    lua_rawset(L, -3);
    // nuke.Packages.read(rel) — content bytes as a Lua string (raw project or mounted pak); nil when absent.
    lua_newtable(L);
    lua_pushcfunction(L, +[](lua_State* LL) -> int {
        std::string data;
        if (!AppInstance::GetSingleton()->ReadContent(luaL_checkstring(LL, 1), data))
        {
            lua_pushnil(LL);
            return 1;
        }
        lua_pushlstring(LL, data.data(), data.size());
        return 1;
    });
    lua_setfield(L, -2, "read");
    lua_pushstring(L, "Packages");
    lua_insert(L, -2);
    lua_rawset(L, -3);
    // Reflected ENUM tables: nuke.<EnumName>.<Label> = value (e.g. nuke.WindowMode.ExclusiveFullscreen).
    // A [[nuke::func]] enum parameter takes the plain int, so scripts pass nuke.WindowMode.X.
    for (const std::string& en : Reflect_AllEnumNames())
        if (const std::vector<std::string>* labels = Reflect_EnumLabels(en))
        {
            lua_newtable(L);
            for (size_t i = 0; i < labels->size(); ++i)
            {
                lua_pushinteger(L, (lua_Integer)i);
                lua_setfield(L, -2, (*labels)[i].c_str());
            }
            lua_pushstring(L, en.c_str());
            lua_insert(L, -2);
            lua_rawset(L, -3);   // nuke[EnumName] = { Label = value, ... }
        }
    lua_pop(L, 1);
}

// Bound method call for an object reached through a live pointer (LuaBridge class userdata):
// upvalues = (obj lightuserdata, typeName, methodName). The closure is made per-access and
// used immediately — it borrows the pointer binding's lifetime, so never store it.
static int ObjMethodCall(lua_State* L)
{
    void* obj = lua_touserdata(L, lua_upvalueindex(1));
    const char* tname = lua_tostring(L, lua_upvalueindex(2));
    const char* mname = lua_tostring(L, lua_upvalueindex(3));
    TypeInfo* ti = Registry_Find(tname);
    const Method* m = ti ? Reflect_FindMethod(ti, mname) : nullptr;
    if (!m || !obj) return luaL_error(L, "%s.%s vanished", tname ? tname : "?", mname ? mname : "?");

    const int base = lua_isuserdata(L, 1) ? 2 : 1;   // colon call: self sits at slot 1
    const size_t nargs = m->params.size();
    if ((size_t)(lua_gettop(L) - base + 1) < nargs)
        return luaL_error(L, "%s:%s expects %d argument(s)", tname, mname, (int)nargs);
    std::vector<ReflectValue> args(nargs);
    for (size_t i = 0; i < nargs; ++i)
    {
        ReflectValue blank; blank.type = m->params[i];
        if (!ReadReflectValue(L, base + (int)i, m->params[i], blank, args[i]))
            return luaL_error(L, "%s:%s: bad argument #%d", tname, mname, (int)i + 1);
    }
    ReflectValue ret;
    if (!Reflect_Invoke(obj, *m, args.data(), nargs, ret))
        return luaL_error(L, "%s:%s: invoke failed", tname, mname);
    if (ret.type == FT::Unknown) return 0;
    return PushReflectValue(L, ret);
}

// __index over the reflection registry for any reflected object exposed as a LuaBridge class:
// fields by name (FT::Vec3 as a LIVE Vector3* so `t.position.y = 1` mutates in place) and
// [[nuke::func]] methods as bound closures.
static lb::LuaRef ReflectedIndex(void* obj, TypeInfo* ti, const lb::LuaRef& key, lua_State* L)
{
    if (!ti || !key.isString()) return lb::LuaRef(L);
    const std::string k = key.tostring();
    if (const Field* f = Reflect_FindField(ti, k))
    {
        if (f->type == FT::Vec3)
        {
            if (!lb::push(L, (Vector3*)f->addr(obj))) lua_pushnil(L);
        }
        else
            PushReflectValue(L, Reflect_GetField(obj, *f));
        lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
        lua_pop(L, 1);
        return ref;
    }
    if (Reflect_FindMethod(ti, k))
    {
        lua_pushlightuserdata(L, obj);
        lua_pushstring(L, ti->name.c_str());
        lua_pushstring(L, k.c_str());
        lua_pushcclosure(L, ObjMethodCall, 3);
        lb::LuaRef ref = lb::LuaRef::fromStack(L, -1);
        lua_pop(L, 1);
        return ref;
    }
    return lb::LuaRef(L);   // unknown name: nil
}

// __newindex over the registry: reflected field writes (t.position = v, t.eulerHint = {…}).
static lb::LuaRef ReflectedNewIndex(void* obj, TypeInfo* ti, const lb::LuaRef& key,
                                    const lb::LuaRef& value, lua_State* L)
{
    if (!ti || !key.isString()) return lb::LuaRef(L);
    const std::string k = key.tostring();
    const Field* f = Reflect_FindField(ti, k);
    if (!f) { luaL_error(L, "%s has no property '%s'", ti->name.c_str(), k.c_str()); return lb::LuaRef(L); }
    value.push();
    ReflectValue v;
    const bool ok = ReadReflectValue(L, -1, f->type, Reflect_GetField(obj, *f), v);
    lua_pop(L, 1);
    if (!ok) { luaL_error(L, "%s.%s: bad value", ti->name.c_str(), k.c_str()); return lb::LuaRef(L); }
    Reflect_SetField(obj, *f, v);
    return lb::LuaRef(L);
}

static void BindEngineAPI(lua_State* L)
{
    RegisterComponentProxy(L);
    RegisterObjectProxy(L);
    lb::getGlobalNamespace(L)
        .beginNamespace("nuke")
            .beginClass<Vector3>("Vector3")
                .addConstructor<void(double, double, double)>()
                .addProperty("x",
                    [](const Vector3* v) { return v->x; },
                    [](Vector3* v, double d) { v->x = d; })
                .addProperty("y",
                    [](const Vector3* v) { return v->y; },
                    [](Vector3* v, double d) { v->y = d; })
                .addProperty("z",
                    [](const Vector3* v) { return v->z; },
                    [](Vector3* v, double d) { v->z = d; })
            .endClass()
            // Transform has no hand-written members: everything dispatches through reflection.
            .beginClass<Transform>("Transform")
                .addIndexMetaMethod(+[](Transform& t, const lb::LuaRef& key, lua_State* LL) {
                    return ReflectedIndex(&t, &TypeOf<Transform>(), key, LL);
                })
                .addNewIndexMetaMethod(+[](Transform& t, const lb::LuaRef& key, const lb::LuaRef& value, lua_State* LL) {
                    return ReflectedNewIndex(&t, &TypeOf<Transform>(), key, value, LL);
                })
            .endClass()
            .beginClass<Atom>("Atom")
                .addProperty("transform",
                    [](Atom* a) -> Transform* { return &a->GetTransform(); })
                .addProperty("name",
                    [](const Atom* a) { return a->name; },
                    [](Atom* a, const std::string& n) { a->name = n; })
                .addProperty("tag",
                    [](const Atom* a) { return a->tag; },
                    [](Atom* a, const std::string& t) { a->tag = t; })
                // Any reflected component works by type name: self:getComponent("Light").
                .addFunction("getComponent",
                    [](Atom* a, const char* type, lua_State* L) -> lb::LuaRef {
                        Component* c = Reflect_FindComponent(a, type ? type : "");
                        return c ? MakeComponentRef(L, a, c) : lb::LuaRef(L);
                    })
                .addFunction("addComponent",
                    [](Atom* a, const char* type, lua_State* L) -> lb::LuaRef {
                        Component* c = Reflect_AddComponent(a, type ? type : "");
                        return c ? MakeComponentRef(L, a, c) : lb::LuaRef(L);
                    })
                // Everything else falls back to the reflected [[nuke::func]] Atom API.
                .addIndexMetaMethod(+[](Atom& a, const lb::LuaRef& key, lua_State* LL) {
                    return ReflectedIndex(&a, &TypeOf<Atom>(), key, LL);
                })
            .endClass()
            // LEGACY aliases (older scripts): the reflected surface is nuke.Time.Elapsed()/Delta().
            .addFunction("time",  [] { return Time::Elapsed(); })
            .addFunction("delta", [] { return Time::Delta(); })
            // Every component type a script can getComponent/addComponent (reflection registry).
            .addFunction("componentTypes",
                [](lua_State* L) -> lb::LuaRef {
                    lb::LuaRef t = lb::newTable(L);
                    int i = 1;
                    for (const std::string& n : Reflect_ComponentTypes()) t[i++] = n;
                    return t;
                })
            // Empty atom reference for a prop default: `props = { target = nuke.AtomRef() }`.
            // Once assigned the prop value is the live Atom; unset/dead refs read as this
            // sentinel table (it has no .transform, so falsy checks work).
            .addFunction("AtomRef",
                [](lua_State* L) -> lb::LuaRef {
                    lb::LuaRef t = lb::newTable(L);
                    t["__atomref"] = 0;
                    return t;
                })
        .endNamespace()
        // Legacy alias namespace for older scripts; the reflected surface is nuke.Gui.*.
        .beginNamespace("gui")
            .addFunction("begin",     [](const char* n) { return nuke::GUI()->Begin(n); })
            .addFunction("done",      [] { nuke::GUI()->End(); })
            .addFunction("text",      [](const char* s) { nuke::GUI()->Text(s); })
            .addFunction("button",    [](const char* s) { return nuke::GUI()->Button(s); })
            .addFunction("sameLine",  [] { nuke::GUI()->SameLine(); })
            .addFunction("separator", [] { nuke::GUI()->Separator(); })
            .addFunction("checkbox",  [](const char* l, bool v)  { nuke::GUI()->Checkbox(l, &v); return v; })
            .addFunction("slider",    [](const char* l, float v, float lo, float hi) { nuke::GUI()->SliderFloat(l, &v, lo, hi); return v; })
        .endNamespace();

    BindReflectedStatics(L);   // nuke.<Type>.<Fn> for every [[nuke::func]] static (e.g. nuke.Physics.Raycast)
}

static void EnsureLua()
{
    if (gL) return;
    gL = luaL_newstate();
    luaL_openlibs(gL);
    BindEngineAPI(gL);
    cout << "[NukeScript]\tLua VM ready." << endl;
}

static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Lua-backed component: loads the script table and drives its update/gui/event hooks.
class ScriptComponent : public Component
{
    NUKE_CLASS(ScriptComponent, Component, "Scripts")
public:
    [[nuke::prop(asset="script")]] std::string script;   // path to a .lua file (asset picker)
    [[nuke::prop(hidden)]]         std::string props;    // edited prop values as JSON (serialized, drawn by OnInspector)

    ScriptComponent() : Component("ScriptComponent"), script("scripts/bob.lua") {}

    void Init(Atom* parent) override
    {
        atom = parent;
        transform = &parent->GetTransform();
        parent->components.push_back(this);
    }

    void Update() override
    {
        if (!EnsureLoaded()) return;
        RunStartOnce();
        if (!table) return;   // start error tore the chunk down
        double dt = Time::getSingleton()->gameDelta;   // scaled game delta: 0 while frozen
        lb::LuaRef upd = (*table)["update"];
        if (upd.isFunction())
        {
            try { upd(atom, dt); }
            catch (const lb::LuaException& e)
            {
                cerr << "[NukeScript]\tupdate error: " << e.what() << endl;
                Clear();
            }
        }
    }

    // Calls the script's `fixedUpdate(self, dt)` at the world's fixedDt cadence. Runs on the
    // fixed thread under the game lock — the lock every VM entry takes, keeping the VM safe.
    void FixedUpdate() override
    {
        if (!EnsureLoaded()) return;
        RunStartOnce();   // whichever cadence reaches the fresh chunk first fires it
        if (!table) return;
        lb::LuaRef fu = (*table)["fixedUpdate"];
        if (!fu.isFunction()) return;
        World* w = AppInstance::GetSingleton()->currentWorld;
        double dt = (w && w->settings.fixedDt > 0.0001f) ? w->settings.fixedDt : 1.0 / 60.0;
        try { fu(atom, dt); }
        catch (const lb::LuaException& e)
        {
            cerr << "[NukeScript]\tfixedUpdate error: " << e.what() << endl;
            Clear();
        }
    }

    // Contact hooks -> script's onCollision*/onTrigger*(self, other). Dispatched by the
    // fixed thread under the game lock, so the VM is entered directly.
    void OnCollisionEnter(Atom* other) override { CallContactHook("onCollisionEnter", other); }
    void OnCollisionExit(Atom* other) override  { CallContactHook("onCollisionExit", other); }
    void OnTriggerEnter(Atom* other) override   { CallContactHook("onTriggerEnter", other); }
    void OnTriggerExit(Atom* other) override    { CallContactHook("onTriggerExit", other); }

    // Returns the script's exported props for the editor. Runs on the render thread, so it
    // takes the game lock: the fixed thread may be inside this same VM.
    std::vector<DynProp> DynamicProps() override
    {
        std::vector<DynProp> out;
        World* w = AppInstance::GetSingleton()->currentWorld;
        struct Guard { World* w; ~Guard() { if (w) w->UnlockGame(); } } guard{ w };
        if (w) w->LockGame();
        if (!EnsureLoaded() || !propsTable || propsTable->isNil())
            return out;
        json defs = defaultsJson.empty() ? json::object() : json::parse(defaultsJson, nullptr, false);
        for (lb::Iterator it(*propsTable); !it.isNil(); ++it)
        {
            DynProp p;
            p.name  = it.key().tostring();
            p.value = ToVar(it.value());
            if (p.value.kind == NukeVar::Kind::None) continue;   // unsupported type
            p.def = (defs.is_object() && defs.contains(p.name)) ? JsonToVar(defs[p.name]) : p.value;
            out.push_back(p);
        }
        return out;
    }

    void SetDynamicProp(const std::string& name, const NukeVar& v) override
    {
        World* w = AppInstance::GetSingleton()->currentWorld;
        struct Guard { World* w; ~Guard() { if (w) w->UnlockGame(); } } guard{ w };
        if (w) w->LockGame();
        if (!EnsureLoaded() || !propsTable) return;
        switch (v.kind)
        {
            case NukeVar::Kind::Number:  (*propsTable)[name] = v.num; break;
            case NukeVar::Kind::Bool:    (*propsTable)[name] = v.b;   break;
            case NukeVar::Kind::String:  (*propsTable)[name] = v.str; break;
            case NukeVar::Kind::AtomRef: SetAtomRefProp(name, v.refId); break;   // live atom into the table
            default: break;
        }
        EncodeProps();
    }

    void Destroy() override     { Clear(); }
    // Calls the script's gui(self); same thread as Update, so no extra locking.
    void OnGUI() override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef g = (*table)["gui"];
        if (g.isFunction())
        {
            try { g(atom); }
            catch (const lb::LuaException& e) { cerr << "[NukeScript]\tgui error: " << e.what() << endl; Clear(); }
        }
    }

    // Animator event -> script's animEvent(self, name); game thread under the game lock.
    void OnAnimEvent(const char* name) override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef h = (*table)["animEvent"];
        if (h.isFunction())
        {
            try { h(atom, std::string(name ? name : "")); }
            catch (const lb::LuaException& e) { cerr << "[NukeScript]\tanimEvent error: " << e.what() << endl; Clear(); }
        }
    }

    // Event-bus delivery -> script's onEvent(self, name, payload); called from World::Update
    // under the game lock.
    void OnEvent(const std::string& name, const std::string& payload) override
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef h = (*table)["onEvent"];
        if (h.isFunction())
        {
            try { h(atom, name, payload); }
            catch (const lb::LuaException& e) { cerr << "[NukeScript]\tonEvent error: " << e.what() << endl; Clear(); }
        }
    }

    // Captures the live exported props at save time. The script's `saveMode` field picks the
    // policy: "all" (default), "none", or "marked" with `saveFields = {...}` merged over the
    // configured props. A script that never loaded keeps its serialized props untouched.
    void OnBeforeSave() override
    {
        if (!table || !propsTable) return;
        try
        {
            lb::LuaRef m = (*table)["saveMode"];
            const std::string mode = m.isString() ? *m.cast<std::string>() : std::string("all");
            if (mode == "none") return;
            if (mode == "marked")
            {
                lb::LuaRef list = (*table)["saveFields"];
                if (!list.isTable()) return;
                json cur = props.empty() ? json::object() : json::parse(props, nullptr, false);
                if (!cur.is_object()) cur = json::object();
                for (int i = 1; ; ++i)
                {
                    lb::LuaRef k = list[i];
                    if (k.isNil()) break;
                    if (!k.isString()) continue;
                    const std::string key = *k.cast<std::string>();
                    lb::LuaRef v = (*propsTable)[key];
                    switch (v.type())
                    {
                        case LUA_TNUMBER:  cur[key] = *v.cast<double>(); break;
                        case LUA_TBOOLEAN: cur[key] = *v.cast<bool>(); break;
                        case LUA_TSTRING:  cur[key] = *v.cast<std::string>(); break;
                        default: break;
                    }
                }
                props = cur.dump();
                return;
            }
            EncodeProps();   // "all": the whole live props table
        }
        catch (const lb::LuaException& e) { cerr << "[NukeScript]\tOnBeforeSave error: " << e.what() << endl; }
    }

    void Pause() override       {}
    void Reset() override       { Clear(); }

private:
    lb::LuaRef* table = nullptr;        // chunk's returned table
    lb::LuaRef* propsTable = nullptr;   // table["props"] (the exported props)
    bool started = false;               // `start` hook fired for the current chunk
    std::string loadedScript;           // path the current chunk came from (for reload-on-change)
    std::string defaultsJson;           // script's original prop defaults (for the reset button)

    // Direct contact-hook dispatch (fixed thread, game lock held by the caller).
    void CallContactHook(const char* fnName, Atom* other)
    {
        if (!EnsureLoaded()) return;
        lb::LuaRef fn = (*table)[fnName];
        if (!fn.isFunction()) return;
        try { fn(atom, other); }
        catch (const lb::LuaException& ex)
        {
            cerr << "[NukeScript]\t" << fnName << " error: " << ex.what() << endl;
        }
    }

    // Fires the script's `start(self)` once per loaded chunk, before the first update.
    // Clear() resets the flag, so a reload runs start again.
    void RunStartOnce()
    {
        if (started || !table) return;
        started = true;
        lb::LuaRef st = (*table)["start"];
        if (!st.isFunction()) return;
        try { st(atom); }
        catch (const lb::LuaException& e)
        {
            cerr << "[NukeScript]\tstart error: " << e.what() << endl;
            Clear();
        }
    }

    void Clear()
    {
        delete table;      table = nullptr;
        delete propsTable; propsTable = nullptr;
        started = false;
    }

    // Load (or reload) the chunk; returns true if a valid table is ready.
    bool EnsureLoaded()
    {
        EnsureLua();
        // Each path is attempted once, success or failure — otherwise a broken script logs every frame.
        if (script == loadedScript)
            return table != nullptr;
        Clear();
        loadedScript = script;

        // Read through the content layers: a packed game serves scripts from a pak in memory.
        std::string resolved = script;
        std::string src;
        AppInstance::GetSingleton()->ReadContent(script, src);
        if (src.empty())
        {
            cerr << "[NukeScript]\tcannot read script '" << script << "' (resolved: " << resolved << ")" << endl;
            return false;
        }
        if (luaL_dostring(gL, src.c_str()) != LUA_OK)
        {
            cerr << "[NukeScript]\tload error in '" << script << "': " << lua_tostring(gL, -1) << endl;
            lua_pop(gL, 1);
            return false;
        }
        table = new lb::LuaRef(lb::LuaRef::fromStack(gL, -1));
        lua_pop(gL, 1);

        lb::LuaRef p = (*table)["props"];
        if (p.isTable())
        {
            propsTable = new lb::LuaRef(p);
            defaultsJson = TableToJson(propsTable).dump();   // capture script defaults first
            ApplySavedProps();                                // then overlay saved edits
        }
        PublishClass();   // this script + its props enter the SHARED reflection registry
        return true;
    }

    // Publish the loaded script as a script CLASS: reflection-driven editor UI (the animation
    // and sequencer prop pickers) then offers its props exactly like native reflected ones.
    void PublishClass()
    {
        ScriptClass sc;
        sc.name = sc.selector = script;
        sc.component = "ScriptComponent";
        if (propsTable && !propsTable->isNil())
            for (lb::Iterator it(*propsTable); !it.isNil(); ++it)
            {
                ScriptProp sp;
                sp.name = it.key().tostring();
                const NukeVar v = ToVar(it.value());
                if      (v.kind == NukeVar::Kind::Number) sp.type = FT::Double;
                else if (v.kind == NukeVar::Kind::Bool)   sp.type = FT::Bool;
                else if (v.kind == NukeVar::Kind::String) sp.type = FT::String;
                else continue;   // atom refs and tables are not keyable values
                sc.props.push_back(sp);
            }
        Reflect_RegisterScriptClass(sc);
    }

    // Atom id behind a prop value: a live Atom, or the `{ __atomref = <id> }` sentinel.
    // Returns -1 when the value is not an atom ref at all.
    static long long AtomRefId(const lb::LuaRef& val)
    {
        if (val.isUserdata() && val.isInstance<Atom>())
        {
            auto a = val.cast<Atom*>();
            return (a && *a) ? (long long)Reflect_AtomId(*a) : 0;
        }
        if (val.isTable() && !val["__atomref"].isNil())
        {
            lb::LuaRef id = val["__atomref"];
            return id.isNumber() ? (long long)*id.cast<double>() : 0;
        }
        return -1;
    }

    static json TableToJson(lb::LuaRef* t)
    {
        json j = json::object();
        if (!t) return j;
        for (lb::Iterator it(*t); !it.isNil(); ++it)
        {
            std::string key = it.key().tostring();
            lb::LuaRef val = it.value();
            const long long ref = AtomRefId(val);
            if (ref >= 0) { j[key] = { { "__atomref", ref } }; continue; }   // by STABLE id
            switch (val.type())
            {
                case LUA_TNUMBER:  j[key] = *val.cast<double>(); break;
                case LUA_TBOOLEAN: j[key] = *val.cast<bool>(); break;
                case LUA_TSTRING:  j[key] = *val.cast<std::string>(); break;
                default: break;
            }
        }
        return j;
    }

    static NukeVar ToVar(lb::LuaRef val)
    {
        NukeVar nv;
        const long long ref = AtomRefId(val);
        if (ref >= 0) { nv.kind = NukeVar::Kind::AtomRef; nv.refId = ref; return nv; }
        switch (val.type())
        {
            case LUA_TNUMBER:  nv.kind = NukeVar::Kind::Number; nv.num = *val.cast<double>(); break;
            case LUA_TBOOLEAN: nv.kind = NukeVar::Kind::Bool;   nv.b   = *val.cast<bool>(); break;
            case LUA_TSTRING:  nv.kind = NukeVar::Kind::String; nv.str = *val.cast<std::string>(); break;
            default: break;
        }
        return nv;
    }

    static NukeVar JsonToVar(const json& v)
    {
        NukeVar nv;
        if (v.is_object() && v.contains("__atomref"))
        {
            nv.kind = NukeVar::Kind::AtomRef;
            nv.refId = v["__atomref"].is_number() ? v["__atomref"].get<long long>() : 0;
        }
        else if (v.is_number())  { nv.kind = NukeVar::Kind::Number; nv.num = v.get<double>(); }
        else if (v.is_boolean()) { nv.kind = NukeVar::Kind::Bool;   nv.b   = v.get<bool>(); }
        else if (v.is_string())  { nv.kind = NukeVar::Kind::String; nv.str = v.get<std::string>(); }
        return nv;
    }

    // Store an atom-ref value into the props table: the LIVE atom when it resolves (the
    // script then uses it directly), else the sentinel keeping the id for later.
    void SetAtomRefProp(const std::string& key, long long id)
    {
        World* w = AppInstance::GetSingleton()->currentWorld;
        Atom* a = (id != 0 && w) ? w->GetById((long)id) : nullptr;
        if (a) (*propsTable)[key] = a;
        else
        {
            lb::LuaRef t = lb::newTable(gL);
            t["__atomref"] = (double)id;
            (*propsTable)[key] = t;
        }
    }

    void SetProp(const std::string& key, const json& v)
    {
        if (!propsTable) return;
        if (v.is_object() && v.contains("__atomref"))
            SetAtomRefProp(key, v["__atomref"].is_number() ? v["__atomref"].get<long long>() : 0);
        else if (v.is_number())  (*propsTable)[key] = v.get<double>();
        else if (v.is_boolean()) (*propsTable)[key] = v.get<bool>();
        else if (v.is_string())  (*propsTable)[key] = v.get<std::string>();
    }

    // propsTable -> `props` JSON (so reflection serializes the edited values).
    void EncodeProps() { if (propsTable) props = TableToJson(propsTable).dump(); }

    // `props` JSON -> propsTable (overlay onto the script's defaults).
    void ApplySavedProps()
    {
        if (!propsTable || props.empty()) return;
        json j = json::parse(props, nullptr, false);
        if (!j.is_object()) return;
        for (auto& kv : j.items()) SetProp(kv.key(), kv.value());
    }
};

// Generated registration for this file's reflected components; must be included IN THIS TU,
// after the class definitions, so the member pointers resolve.
#include "NukeScript.gen.inc"   // defines NukeReflectInit_NukeScript()

// iScript service ("scripting"): snippets run in the SAME shared VM as ScriptComponents.
struct LuaScriptService : public iScript
{
    const char* Language() override { return "lua"; }
    const char* HostComponent() override { return "ScriptComponent"; }
    const char* Icon() override { return ICON_FT_LUA; }

    // This backend's classes ARE the project's scripts: a listing (no chunk runs, no caching,
    // no watcher) answered when someone asks. Extension and layout are this module's business,
    // which is why the engine never spells either out.
    int ListClasses(char* buf, int cap) override
    {
        AppInstance* app = AppInstance::GetSingleton();
        if (!app) return 0;
        std::set<std::string> rel;                  // sorted + de-duped across layers
        boost::system::error_code ec;
        const bfs::path croot(app->contentRoot);
        auto isLua = [](std::string e)
        {
            for (char& c : e) c = (char)tolower((unsigned char)c);
            return e == ".lua";
        };
        if (!croot.empty() && bfs::exists(croot, ec))
            for (bfs::recursive_directory_iterator it(croot, ec), end; it != end; it.increment(ec))
            {
                if (ec) break;
                if (bfs::is_directory(it->path(), ec) || !isLua(it->path().extension().string())) continue;
                rel.insert(bfs::relative(it->path(), croot, ec).generic_string());
            }
        // packed session: the scripts live in mounted paks instead
        if (Package::MountedCount() > 0)
            for (const std::string& pr : Package::List("content/"))
                if (isLua(bfs::path(pr).extension().string()))
                    rel.insert(pr.substr(strlen("content/")));
        std::string out;
        for (const std::string& r : rel) out += r + '\n';
        if (out.empty()) return 0;
        if (buf && cap >= (int)out.size()) memcpy(buf, out.data(), out.size());
        return (int)out.size();
    }

    // The props of ONE script, asked for by the editor when the user picks that script (no
    // background work, no project sweep): the chunk loads into a scratch state that only
    // returns its table, and the `props` entries become "name<TAB>kind" lines.
    int ListClassProps(const char* cls, char* buf, int cap) override
    {
        AppInstance* app = AppInstance::GetSingleton();
        std::string src;
        if (!app || !cls || !*cls || !app->ReadContent(cls, src)) return 0;
        lua_State* L = luaL_newstate();
        if (!L) return 0;
        luaL_openlibs(L);
        std::string out;
        if (luaL_loadbuffer(L, src.data(), src.size(), cls) == LUA_OK
            && lua_pcall(L, 0, 1, 0) == LUA_OK && lua_istable(L, -1))
        {
            lua_getfield(L, -1, "props");
            if (lua_istable(L, -1))
            {
                lua_pushnil(L);
                while (lua_next(L, -2))
                {
                    if (lua_type(L, -2) == LUA_TSTRING)
                    {
                        const int vt = lua_type(L, -1);
                        const char* kind = vt == LUA_TNUMBER  ? "number"
                                         : vt == LUA_TBOOLEAN ? "bool"
                                         : vt == LUA_TSTRING  ? "string" : nullptr;
                        if (kind) { out += lua_tostring(L, -2); out += '\t'; out += kind; out += '\n'; }
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_close(L);
        if (out.empty()) return 0;
        if (buf && cap >= (int)out.size()) memcpy(buf, out.data(), out.size());
        return (int)out.size();
    }

    bool Run(const char* code, const char* chunkName) override
    {
        if (!code) return false;
        EnsureLua();
        if (luaL_loadbuffer(gL, code, strlen(code), chunkName ? chunkName : "snippet") != LUA_OK
            || lua_pcall(gL, 0, 0, 0) != LUA_OK)
        {
            cerr << "[NukeScript]\tRun('" << (chunkName ? chunkName : "snippet")
                 << "') error: " << lua_tostring(gL, -1) << endl;
            lua_pop(gL, 1);
            return false;
        }
        return true;
    }
};
static LuaScriptService gScriptService;

struct NukeScriptModule : public NUKEModule
{
    NukeScriptModule()
    {
        strcpy(title, "NukeScript");
        strcpy(author, "Luastris");
        strcpy(description, "Lua scripting: ScriptComponent with inspector-editable props.");
        strcpy(version, "1.0.0.0");
        strcpy(site, "https://luastris.com");
        tags = { "lua", "scripting", "gameplay" };
    }

    // Scripting is a SHARED service: several backends may be live at once, so consumers use
    // GetServices<iScript>() as well as GetService<iScript>().
    const char* provides() override { return "scripting"; }
    void*       queryService() override { return static_cast<iScript*>(&gScriptService); }
    bool        sharedService() override { return true; }

    // Shipping cooker: claims .lua and reports every quoted literal as a potential asset
    // reference for the editor to resolve. Composed strings need "packInclude" in the .nuproj.
    bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
                     std::vector<std::string>& outUses) override
    {
        std::string rel = contentRel ? contentRel : "";
        for (char& c : rel) c = (char)tolower((unsigned char)c);
        if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, ".lua") != 0) return false;
        const char* src = bytes;
        for (uint64_t i = 0; src && i < size; ++i)
        {
            char q = src[i];
            if (q != '"' && q != '\'') continue;
            uint64_t e = i + 1;
            while (e < size && src[e] != q && src[e] != '\n') ++e;
            if (e < size && src[e] == q)
            {
                if (e - i > 1 && e - i < 512) outUses.emplace_back(src + i + 1, (size_t)(e - i - 1));
                i = e;
            }
        }
        return true;   // .lua is ours — it ships (with the scripting module present)
    }

    // Activation hook, called synchronously before Run: registers ScriptComponent and the
    // .lua asset type, so they exist only while the plugin is enabled.
    void OnLoad() override
    {
        NukeReflectInit_NukeScript();   // register this module's reflected components (generated)
        cout << "[NukeScript]\tScriptComponent registered." << endl;
        // File-type descriptor for .lua; the editor does the actual file IO.
        nuke::AssetCreator luaType;
        luaType.label = "Lua Script";
        luaType.ext = ".lua";
        luaType.icon = ICON_FT_LUA;   // this module owns the type, so it names the glyph
        luaType.baseName = "New Script";
        luaType.category = "Scripts";
        luaType.textEditable = true;
        luaType.syntaxLanguage = "lua";
        luaType.content =
            "-- NukeEngine Lua script\n"
            "return {\n"
            "    props = {\n"
            "        -- speed = 1.0,\n"
            "    },\n"
            "    update = function(self, dt)\n"
            "        -- self is the atom; any reflected component works by type name:\n"
            "        -- local light = self:getComponent(\"Light\")\n"
            "        -- if light then light.intensity = 5 end\n"
            "        -- Assets are OBJECTS (create/find/assign by name — never a guid):\n"
            "        -- local mr = self:getComponent(\"MeshRenderer\")\n"
            "        -- mr.mesh = nuke.Mesh.Find(\"builtin:sphere\")\n"
            "        -- mr.material.shader = nuke.Shader.Find(\"world\")\n"
            "        -- local tex = nuke.Texture.Create()\n"
            "        -- tex:setPixels(w, h, rgbaString)   -- #rgbaString == w*h*4\n"
            "        -- mr.material.diffuse = tex\n"
            "        -- Content bytes (raw project or pak): nuke.Packages.read(\"path\")\n"
            "    end,\n"
            "    -- Fixed-rate tick (physics cadence, frame-independent):\n"
            "    -- fixedUpdate = function(self, dt) end,\n"
            "    -- Physics hooks (need a Collider on this atom):\n"
            "    -- onCollisionEnter = function(self, other) end,\n"
            "    -- onCollisionExit  = function(self, other) end,\n"
            "    -- onTriggerEnter   = function(self, other) end,\n"
            "    -- onTriggerExit    = function(self, other) end,\n"
            "    -- Ray cast (reflected facade, nuke.Physics.*):\n"
            "    -- if nuke.Physics.Raycast(from, dir, 100) then\n"
            "    --     local a = nuke.Physics.HitAtom(); local d = nuke.Physics.HitDistance()\n"
            "    -- end\n"
            "    -- Runtime UI (drawn while playing); always pair begin/done.\n"
            "    gui = function(self)\n"
            "        gui.begin(\"Script UI\")\n"
            "        gui.text(\"hello from lua\")\n"
            "        gui.done()\n"
            "    end,\n"
            "}\n";
        nuke::RegisterAssetCreator(luaType);
    }

    void Run(AppInstance* inst) override
    {
        instance = inst;
        stopped = false;
        while (!stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool HasSettings() override { return false; }
    void Settings() override {}
    void Shutdown() override { stopped = true; }
};

extern "C" BOOST_SYMBOL_EXPORT NukeScriptModule plugin;
NukeScriptModule plugin;
