/*
  ==============================================================================

   This file is part of the LUBYK project (http://lubyk.org)
   Copyright (c) 2007-2011 by Gaspard Bucher (http://teti.ch).

  ------------------------------------------------------------------------------

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.

  ==============================================================================
*/
#include "osc/osc.h"

#include "osc/OscReceivedElements.h"
#include "osc/OscOutboundPacketStream.h"

#include "dub/dub.h"

#define MAX_DEPTH 10000
#define MAX_BUFF_SIZE 8196

#if LUA_VERSION_NUM > 501
#define COMPAT_GETN luaL_len
#else
#define COMPAT_GETN luaL_getn
#endif

static const char *parse_osc_value(lua_State *L, const char *start_type_tags, osc::ReceivedMessage::const_iterator &arg);
static void pack_lua(lua_State *L, osc::OutboundPacketStream &pk, int index);

static void pack_string(lua_State *L, osc::OutboundPacketStream &pk, int index) {
  size_t sz;
  const char* str = lua_tolstring(L, index, &sz);
  if (!str) {
    int type = lua_type(L, index);
    fprintf(stderr, "Could not serialize string at index %d (%s found)\n", index, lua_typename(L, type));
    return;
  }
  pk << str;
}

static void pack_array(lua_State *L, osc::OutboundPacketStream &pk, int index, size_t sz) {
  // do not insert array markers for level 0 type tags: "ff[fs]" not "[ff[fs]]"
  pk << osc::BeginArray;
    for (size_t i = 1; i <= sz; ++i) {
      lua_rawgeti(L, index, i);
      pack_lua(L, pk, -1);
      lua_pop(L, 1);
    }
  pk << osc::EndArray;
}

static void pack_hash(lua_State *L, osc::OutboundPacketStream &pk, int index) {
  pk << osc::BeginHash;
  // ... <table> ...
  for(lua_pushnil(L); lua_next(L, index) != 0; lua_pop(L, 1)) {
    // push key
    pack_lua(L, pk, -2);
    // push value
    pack_lua(L, pk, -1);
  }
  pk << osc::EndHash;
}

static void pack_table(lua_State *L, osc::OutboundPacketStream &pk, int index) {
  if (index > MAX_DEPTH) {
    // ERROR
    throw dub::Exception("Cannot send table (recursive or too large).");
  }

  size_t sz = COMPAT_GETN(L, index);

  if (sz > 0) {
    pack_array(L, pk, index, sz);
  } else {
    pack_hash(L, pk, index);
  }
}

static void pack_lua(lua_State *L, osc::OutboundPacketStream &pk, int index) {
  if (index < 0) {
    // resolve negative index so that it is consistent on recursive table handling
    index = lua_gettop(L) + index + 1;
  }
  int type = lua_type(L, index);
  switch (type) {
  case LUA_TNUMBER:
    pk << (float)lua_tonumber(L, index);
    return;
  case LUA_TBOOLEAN:
    if (lua_toboolean(L, index)) {
      pk << true;
    } else {
      pk << false;
    }
    return;
  case LUA_TSTRING:
    return pack_string(L, pk, index);
  case LUA_TNIL:
    pk << osc::Nil;
    return;
  case LUA_TTABLE:
    return pack_table(L, pk, index);
  case LUA_TUSERDATA:
    // TODO: support serialization (trying __serialize method?)
    // memory leak (msgpack_packer and buffer not freed if we use luaL_error)
  case LUA_TTHREAD:
  case LUA_TLIGHTUSERDATA:
  default:
    throw dub::Exception("Cannot pack message of type %s.", lua_typename(L, type));
  }
}

/** Parse a list of elements and advance 'arg' and type tags.
 * sz is only provided if we want to treat the array as an arglist.
 */
static const char *parse_osc_array(lua_State *L, const char *start_type_tags, osc::ReceivedMessage::const_iterator &arg, size_t *sz = NULL) {
  size_t i = 0;
  const char *type_tags = start_type_tags;
  if (!type_tags) return 0;

  if (sz) {
    // we do not create a table but simply push values on the stack
    while (*type_tags) {
      ++i;
      type_tags = parse_osc_value(L, type_tags, arg);
      // do not eat ending ']'
      if (*type_tags == osc::ARRAY_END_TYPE_TAG) break;
    }
    *sz = i;
  } else {
    lua_newtable(L);
    size_t tbl_pos = lua_gettop(L);
    while (*type_tags) {
      ++i;
      type_tags = parse_osc_value(L, type_tags, arg);
      lua_rawseti(L, tbl_pos, i);
      // do not eat ending ']'
      if (*type_tags == osc::ARRAY_END_TYPE_TAG) break;
    }
  }
  return type_tags;
}

/** Parse a hash and advance 'arg' and type tags.
 */
static const char *parse_osc_hash(lua_State *L, const char *start_type_tags, osc::ReceivedMessage::const_iterator &arg) {
  lua_newtable(L);
  size_t tablepos = lua_gettop(L);
  const char *type_tags = start_type_tags;

  while (*type_tags) {
    if (*type_tags == osc::STRING_TYPE_TAG) {
      // Get key
      lua_pushstring(L, arg->AsStringUnchecked());
      ++type_tags;
      ++arg;

      type_tags = parse_osc_value(L, type_tags, arg);
      lua_rawset(L, tablepos);
    } else if (*type_tags == osc::HASH_END_TYPE_TAG) {
      // finished
      // do not eat ending '}'
      return type_tags;

    } else {
      // mal-formed message: ignore all until HASH_END
      return "";
    }

  }
  return type_tags;
}

/** Parse a single value and advance 'arg' and type tags.
 * A single value can be represented by a single element or enclosed in [...] (Array) or {...} (Hash).
 */
static const char *parse_osc_value(lua_State *L, const char *start_type_tags, osc::ReceivedMessage::const_iterator &arg) {
  const char *type_tags = start_type_tags;

  switch (*type_tags) {
    case osc::TRUE_TYPE_TAG:
      lua_pushboolean(L, true);
      break;
    case osc::FALSE_TYPE_TAG:
      lua_pushboolean(L, false);
      break;
    case osc::NIL_TYPE_TAG:
      lua_pushnil(L);
      break;
      // case osc::INFINITUM_TYPE_TAG:
      //   ??
    case osc::ARRAY_BEGIN_TYPE_TAG:
      // eat opening '['
      ++type_tags;
      ++arg;
      type_tags = parse_osc_array(L, type_tags, arg);
      break;
    case osc::ARRAY_END_TYPE_TAG:
      // return before type_tags increment
      return type_tags;

    case osc::HASH_BEGIN_TYPE_TAG:
      // eat opening '{'
      ++type_tags;
      ++arg;
      type_tags = parse_osc_hash(L, type_tags, arg);
      break;
    case osc::HASH_END_TYPE_TAG:
      // return before type_tags increment
      return type_tags;

    case osc::INT32_TYPE_TAG:
      lua_pushnumber(L, arg->AsInt32Unchecked());
      break;
    case osc::FLOAT_TYPE_TAG:
      lua_pushnumber(L, arg->AsFloatUnchecked());
      break;
    case osc::CHAR_TYPE_TAG:
      lua_pushnumber(L, arg->AsCharUnchecked());
      break;
    case osc::DOUBLE_TYPE_TAG:
      lua_pushnumber(L, arg->AsDoubleUnchecked());
      break;
    case osc::STRING_TYPE_TAG:
      lua_pushstring(L, arg->AsStringUnchecked());
      break;
    case osc::MIDI_MESSAGE_TYPE_TAG:
      {
        lua_newtable(L);
        size_t tablepos = lua_gettop(L);
        osc::uint32 m = arg->AsMidiMessageUnchecked();

        // 3 byte midi message
        lua_pushnumber(L, (int)((m>>16) & 0xFF));
        lua_rawseti(L, tablepos, 1);

        lua_pushnumber(L, (int)((m>>8) & 0xFF));
        lua_rawseti(L, tablepos, 2);

        lua_pushnumber(L, (int)(m & 0xFF));
        lua_rawseti(L, tablepos, 3);
      }
      break;
    case osc::RGBA_COLOR_TYPE_TAG:
    case osc::INT64_TYPE_TAG:
    case osc::TIME_TAG_TYPE_TAG:
    case osc::SYMBOL_TYPE_TAG:
    case osc::BLOB_TYPE_TAG:
    default:
      // TODO
      break;
  }
  ++type_tags;
  ++arg;
  return type_tags;
}


/** Returns a lua table with the bundle content.
 */
static int unpack_bundle(lua_State *L, const osc::ReceivedBundle &b) {
  // foreach message in bundle
  lua_newtable(L);
  size_t tablepos = lua_gettop(L);
  size_t i = 0;
  for (osc::ReceivedBundle::const_iterator elem = b.ElementsBegin(); elem != b.ElementsEnd(); ++elem) {
    if (elem->IsBundle()) {
      unpack_bundle(L, osc::ReceivedBundle(*elem));
      lua_rawseti(L, tablepos, ++i);
    } else {
      // one table with message
      osc::ReceivedMessage m(*elem);
      lua_pushstring(L, m.AddressPattern());
      osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
      // Returns all elements in a table
      parse_osc_array(L, m.TypeTags(), arg);
      lua_rawseti(L, tablepos, ++i);
    }
  }
  return 1;
}

/** Retrieve lua values from an osc message.
 */
static int unpack_packet(lua_State *L, const osc::ReceivedPacket &pk) { 
  if (pk.IsBundle()) {
    return unpack_bundle(L, osc::ReceivedBundle(pk));
  } else {
    osc::ReceivedMessage m(pk);
    lua_pushstring(L, m.AddressPattern());
    osc::ReceivedMessage::const_iterator arg = m.ArgumentsBegin();
    size_t sz = 0;
    parse_osc_array(L, m.TypeTags(), arg, &sz);
    // Url + arguments
    return sz + 1;
  }
}

/** Unpack an osc packet from string data. First argument is the binary data.
 * If there is a bundle, returns a table with all messages.
 */
LuaStackSize osc::unpack(lua_State *L) {
  size_t sz;
  const char *str = dub::checklstring(L, -1, &sz);
  return unpack_packet(L, osc::ReceivedPacket(str, sz));
}

/** Pack arguments into an osc packet. First argument is the url.
 */
LuaStackSize osc::pack(lua_State *L) {
  static char buffer[MAX_BUFF_SIZE];
  // <url> ...
  const char *url = luaL_checkstring(L, 1);
  osc::OutboundPacketStream pk(buffer, MAX_BUFF_SIZE);

  pk << osc::BeginMessage(url);
    size_t top = lua_gettop(L);
    for (size_t i = 2; i <= top; ++i) {
      pack_lua(L, pk, i);
    }
  pk << osc::EndMessage;

  lua_pushlstring(L, pk.Data(), pk.Size());
  return 1;
}


