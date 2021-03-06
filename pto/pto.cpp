﻿#include <vector>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "format.h"
#include "lua.hpp"

#define MAX_DEPTH 128
#define BUFFER_SIZE 128
#define MAX_INT 	0xffffffffffffff

enum eTYPE {
	Bool = 0,
	Short,
	Int,
	Float,
	Double,
	String,
	Pto
};


struct BadPto : std::exception {
	std::string reason_;
};

struct BadArrayType : public BadPto {
	BadArrayType(const char* field, const char* vt) {
		reason_ = fmt::format("field:{} expect table,not {}", field, vt);
	}
};

struct BadArraySize : public BadPto {
	BadArraySize(const char* field) {
		reason_ = fmt::format("field:{} array size more than 0xffff", field);
	}
};

struct BadField : public BadPto {
	BadField(bool array, const char* field, const char* expect, const char* vt) {
		if ( array ) {
			reason_ = fmt::format("field:{} array member expect {},not {}", field, expect, vt);
		}
		else {
			reason_ = fmt::format("field:{} expect {},not {}", field, expect, vt);
		}
	}
};

struct BadInt : public BadPto {
	BadInt(const char* field, lua_Integer val, bool array) {
		if ( array ) {
			reason_ = fmt::format("field:{} array member int out of range,{}", field, val);
		}
		else {
			reason_ = fmt::format("field:{} int out of range,{}", field, val);
		}
	}
};

struct BadString : public BadPto {
	BadString(const char* field, size_t size) {
		reason_ = fmt::format("field:{} string size more than 0xffff:{}", field, size);
	}
};

struct BadType : public BadPto {
	BadType(const char* field, int type) {
		reason_ = fmt::format("unknown field:{},type:{}", field, type);
	}
};

struct BadDecode : public BadPto {
	BadDecode() {
		reason_ = "invalid message";
	}
};

struct TooDepth : public BadPto {
	TooDepth(bool encode) {
		if ( encode ) {
			reason_ = "pto encode too depth";
		}
		else {
			reason_ = "pto decode too depth";
		}
	}
};

struct Field {
	char* name_;
	bool array_;
	eTYPE type_;

	std::vector<Field*> childs_;

	Field(const char* name, bool array, eTYPE type) {
		name_ = strdup(name);
		array_ = array;
		type_ = type;
	}

	~Field() {
		free(name_);
		for ( uint32_t i = 0; i < childs_.size(); ++i ) {
			Field* field = childs_[i];
			delete field;
		}
	}

	inline Field* GetField(uint32_t index) {
		if ( index > childs_.size() ) {
			return NULL;
		}
		return childs_[index];
	}
};

struct Protocol {
	char* name_;
	std::vector<Field*> fields_;

	Protocol(const char* name) {
		name_ = strdup(name);
	}

	~Protocol() {
		free(name_);
		for ( uint32_t i = 0; i < fields_.size(); ++i ) {
			Field* field = fields_[i];
			delete field;
		}
	}

	void AddField(struct Field* field) {
		fields_.push_back(field);
	}

	inline Field* GetField(int index) {
		return fields_[index];
	}
};

struct Context {
	std::vector<Protocol*> ptos_;

	Context() {
		ptos_.resize(0xffff);
	}

	~Context() {
		for ( uint32_t i = 0; i < ptos_.size(); ++i ) {
			Protocol* pto = ptos_[i];
			if ( pto ) {
				delete pto;
			}
		}
	}

	void AddPto(uint16_t id, Protocol* pto) {
		ptos_[id] = pto;
	}

	inline Protocol* GetPto(uint16_t id) {
		if ( id >= 0xffff ) {
			return NULL;
		}
		return ptos_[id];
	}
};

struct Encoder {
	char* ptr_;
	int offset_;
	int size_;
	char init_[BUFFER_SIZE];

	Encoder() {
		ptr_ = init_;
		offset_ = 0;
		size_ = BUFFER_SIZE;
	}

	~Encoder() {
		if ( ptr_ != init_ ) {
			free(ptr_);
		}
	}

	inline void Reserve(int sz) {
		if ( offset_ + sz <= size_ ) {
			return;
		}
		int nsize = size_ * 2;
		while ( nsize < offset_ + sz ) {
			nsize = nsize * 2;
		}

		char* nptr = (char*)malloc(nsize);
		memcpy(nptr, ptr_, size_);
		size_ = nsize;

		if ( ptr_ != init_ ) {
			free(ptr_);
		}
		ptr_ = nptr;
	}

	inline void Append(void* data, int size) {
		Reserve(size);
		memcpy(ptr_ + offset_, data, size);
		offset_ += size;
	}

	inline void Append(const char* str, int sz) {
		Append<uint16_t>(sz);
		Append((void*)str, sz);
	}

	template<typename T>
	inline void Append(T val) {
		Append(&val, sizeof(T));
	}

	inline void Append(int64_t val) {
		if ( val == 0 ) {
			Append((uint8_t)0);
			return;
		}
		uint64_t value;
		uint8_t positive = 0;
		if ( val < 0 ) {
			positive = 0;
			value = -val;
		}
		else {
			positive = 1;
			value = val;
		}

		int length;
		if ( value <= 0xff ) {
			length = 1;
		}
		else if ( value <= 0xffff ) {
			length = 2;
		}
		else if ( value <= 0xffffff ) {
			length = 3;
		}
		else if ( value <= 0xffffffff ) {
			length = 4;
		}
		else if ( value <= 0xffffffffff ) {
			length = 5;
		}
		else if ( value <= 0xffffffffffff ) {
			length = 6;
		}
		else {
			length = 7;
		}

		uint8_t tag = length;
		tag = (tag << 1) | positive;

		uint8_t data[8] = {0};
		data[0] = tag;
		memcpy(&data[1], &value, length);

		Append(data, length + 1);
	}


	void EncodeOne(lua_State* L, Field* field, int index, int depth) {
		switch ( field->type_ ) {
			case eTYPE::Bool:
				EncodeBool(L, field, index);
				break;
			case eTYPE::Short:
				EncodeShort(L, field, index);
				break;
			case eTYPE::Int:
				EncodeInt(L, field, index);
				break;
			case eTYPE::Float:
				EncodeFloat(L, field, index);
				break;
			case eTYPE::Double:
				EncodeDouble(L, field, index);
				break;
			case eTYPE::String:
				EncodeString(L, field, index);
				break;
			case eTYPE::Pto:
				EncodePto(L, field, index, depth);
				break;
			default:
				throw BadType(field->name_, field->type_);
				break;
		}
	}

	inline uint16_t BeginArray(lua_State* L, Field* field, int index, int vt) {
		if ( vt != LUA_TTABLE ) {
			throw BadArrayType(field->name_, lua_typename(L, vt));
		}

		size_t size = lua_rawlen(L, index);
		if ( size > 0xffff ) {
			throw BadArraySize(field->name_);
		}
		Append<uint16_t>(size);
		return size;
	}

	void EncodeBool(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);
		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TBOOLEAN ) {
					throw BadField(true, field->name_, "bool", lua_typename(L, vt));
				}
				Append<bool>(lua_toboolean(L, -1));
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TBOOLEAN ) {
				throw BadField(false, field->name_, "bool", lua_typename(L, vt));
			}
			Append<bool>(lua_toboolean(L, index));
		}
	}

	void EncodeShort(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);
		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TNUMBER ) {
					throw BadField(true, field->name_, "short", lua_typename(L, vt));
				}
				Append<short>((short)lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TNUMBER ) {
				throw BadField(false, field->name_, "short", lua_typename(L, vt));
			}
			Append<short>((short)lua_tointeger(L, index));
		}
	}

	inline void EncodeInt(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);

		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TNUMBER ) {
					throw BadField(true, field->name_, "int", lua_typename(L, vt));
				}
				lua_Integer val = lua_tointeger(L, -1);
				if ( val > MAX_INT || val < -MAX_INT ) {
					throw BadInt(field->name_, val, true);
				}
				Append((int64_t)val);
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TNUMBER ) {
				throw BadField(false, field->name_, "int", lua_typename(L, vt));
			}
			lua_Integer val = lua_tointeger(L, index);
			if ( val > MAX_INT || val < -MAX_INT ) {
				throw BadInt(field->name_, val, false);
			}
			Append((int64_t)val);
		}
	}

	inline void EncodeFloat(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);

		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TNUMBER ) {
					throw BadField(true, field->name_, "float", lua_typename(L, vt));
				}
				Append<float>((float)lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TNUMBER ) {
				throw BadField(false, field->name_, "float", lua_typename(L, vt));
			}
			Append<float>((float)lua_tonumber(L, index));
		}
	}

	inline void EncodeDouble(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);
		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TNUMBER ) {
					throw BadField(true, field->name_, "double", lua_typename(L, vt));
				}
				Append<double>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TNUMBER ) {
				throw BadField(false, field->name_, "double", lua_typename(L, vt));
			}
			Append<double>(lua_tonumber(L, index));
		}
	}

	inline void EncodeString(lua_State* L, Field* field, int index) {
		int vt = lua_type(L, index);
		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 1; i <= size; i++ ) {
				lua_rawgeti(L, index, i);
				vt = lua_type(L, -1);
				if ( vt != LUA_TSTRING ) {
					throw BadField(true, field->name_, "string", lua_typename(L, vt));
				}
				size_t size;
				const char* str = lua_tolstring(L, -1, &size);
				if ( size > 0xffff ) {
					throw BadString(field->name_, size);
				}
				Append(str, size);
				lua_pop(L, 1);
			}
		}
		else {
			if ( vt != LUA_TSTRING ) {
				throw BadField(false, field->name_, "string", lua_typename(L, vt));
			}
			size_t size;
			const char* str = lua_tolstring(L, index, &size);
			if ( size > 0xffff ) {
				throw BadString(field->name_, size);
			}
			Append(str, size);
		}
	}


	inline void EncodePto(lua_State* L, Field* field, int index, int depth) {
		depth++;
		if ( depth > MAX_DEPTH ) {
			throw TooDepth(true);
		}

		int vt = lua_type(L, index);
		if ( vt != LUA_TTABLE ) {
			throw BadField(false, field->name_, "table", lua_typename(L, vt));
		}

		if ( field->array_ ) {
			uint16_t size = BeginArray(L, field, index, vt);
			for ( int i = 0; i < size; i++ ) {
				lua_rawgeti(L, index, i + 1);
				vt = lua_type(L, -1);
				if ( vt != LUA_TTABLE ) {
					throw BadField(true, field->name_, "table", lua_typename(L, vt));
				}

				for ( uint32_t j = 0; j < field->childs_.size(); j++ ) {
					Field* child = field->GetField(j);
					lua_getfield(L, -1, child->name_);
					EncodeOne(L, child, index + 2, depth);
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
			}
		}
		else {
			for ( uint32_t i = 0; i < field->childs_.size(); i++ ) {
				Field* child = field->GetField(i);
				lua_getfield(L, index, child->name_);
				EncodeOne(L, child, index + 1, depth);
				lua_pop(L, 1);
			}
		}
	}
};

struct Decoder {
	const char* ptr_;
	int offset_;
	int size_;

	Decoder(const char* ptr, int size) {
		ptr_ = ptr;
		size_ = size;
		offset_ = 0;
	}

	inline void Read(uint8_t* val, int size) {
		if ( size_ - offset_ < size ) {
			throw BadDecode();
		}
		memcpy(val, ptr_ + offset_, size);
		offset_ += size;
	}

	template<typename T>
	inline T Read()  {
		if ( sizeof(T) > size_ - offset_ ) {
			throw BadDecode();
		}
		T val = *((T*)&ptr_[offset_]);
		offset_ += sizeof(T);
		return val;
	}

	inline int64_t Read() {
		uint8_t tag = Read<uint8_t>();

		if ( tag == 0 ) {
			return 0;
		}

		int length = tag >> 1;

		uint64_t value = 0;
		Read((uint8_t*)&value, length);

		return (tag & 0x1) == 1 ? value : -(lua_Integer)value;
	}

	inline const char* Read(uint16_t* size) {
		*size = Read<uint16_t>();
		if ( size_ - offset_ < *size ) {
			throw BadDecode();
		}
		const char* result = ptr_ + offset_;
		offset_ += *size;
		return result;
	}

	void DecodeOne(lua_State* L, Field* field, int index, int depth) {
		switch ( field->type_ ) {
			case eTYPE::Bool:
				DecodeBool(L, field, index);
				break;
			case eTYPE::Short:
				DecodeShort(L, field, index);
				break;
			case eTYPE::Int:
				DecodeInt(L, field, index);
				break;
			case eTYPE::Float:
				DecodeFloat(L, field, index);
				break;
			case eTYPE::Double:
				DecodeDouble(L, field, index);
				break;
			case eTYPE::String:
				DecodeString(L, field, index);
				break;
			case eTYPE::Pto:
				DecodePto(L, field, index, depth);
				break;
			default:
				throw BadType(field->name_, field->type_);
		}
	}

	inline void DecodeBool(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				bool val = Read<bool>();
				lua_pushboolean(L, val);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			bool val = Read<bool>();
			lua_pushboolean(L, val);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodeShort(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				short val = Read<short>();
				lua_pushinteger(L, val);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			short val = Read<short>();
			lua_pushinteger(L, val);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodeInt(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				int64_t val = Read();
				lua_pushinteger(L, val);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			int64_t val = Read();
			lua_pushinteger(L, val);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodeFloat(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				float val = Read<float>();
				lua_pushnumber(L, val);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			float val = Read<float>();
			lua_pushnumber(L, val);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodeDouble(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				double val = Read<double>();
				lua_pushnumber(L, val);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			double val = Read<double>();
			lua_pushnumber(L, val);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodeString(lua_State* L, Field* field, int index) {
		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				uint16_t size;
				const char* val = Read(&size);
				lua_pushlstring(L, val, size);
				lua_rawseti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			uint16_t size;
			const char* val = Read(&size);
			lua_pushlstring(L, val, size);
			lua_setfield(L, index, field->name_);
		}
	}

	inline void DecodePto(lua_State* L, Field* field, int index, int depth) {
		depth++;
		if ( depth > MAX_DEPTH ) {
			throw TooDepth(false);
		}

		if ( field->array_ ) {
			uint16_t size = Read<uint16_t>();
			lua_createtable(L, 0, 0);
			for ( int i = 1; i <= size; i++ ) {
				int size = field->childs_.size();
				lua_createtable(L, 0, size);
				for ( int j = 0; j < size; j++ ) {
					Field* child = field->GetField(j);
					DecodeOne(L, child, index + 2, depth);
				}
				lua_seti(L, -2, i);
			}
			lua_setfield(L, index, field->name_);
		}
		else {
			int size = field->childs_.size();
			lua_createtable(L, 0, size);
			for ( int i = 0; i < size; i++ ) {
				Field* child = field->GetField(i);
				DecodeOne(L, child, index + 1, depth);
			}
			lua_setfield(L, index, field->name_);
		}
	}

};

static void ImportField(lua_State* L, Context* ctx, std::vector<Field*>& fields, int index, int depth) {
	int size = lua_rawlen(L, index);
	for ( int i = 1; i <= size; i++ ) {
		lua_rawgeti(L, index, i);

		lua_getfield(L, -1, "type");
		eTYPE type = (eTYPE)lua_tointeger(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, -1, "array");
		bool array = lua_toboolean(L, -1);
		lua_pop(L, 1);

		lua_getfield(L, -1, "name");
		const char* name = lua_tostring(L, -1);
		lua_pop(L, 1);

		Field* field = new Field(name, array, type);

		if ( type == eTYPE::Pto ) {
			lua_getfield(L, -1, "pto");
			ImportField(L, ctx, field->childs_, lua_gettop(L), ++depth);
			lua_pop(L, 1);
		}

		fields.push_back(field);

		lua_pop(L, 1);
	}
}

static int ImportPto(lua_State* L) {
	Context* ctx = (Context*)lua_touserdata(L, 1);
	uint16_t id = (uint16_t)luaL_checkinteger(L, 2);
	size_t size;
	const char* name = luaL_checklstring(L, 3, &size);

	luaL_checktype(L, 4, LUA_TTABLE);
	if ( id > 0xffff ) {
		luaL_error(L, "id must less than 0xffff");
	}

	luaL_checkstack(L, MAX_DEPTH * 2 + 8, NULL);

	Protocol* pto = new Protocol(name);

	int depth = 0;
	ImportField(L, ctx, pto->fields_, lua_gettop(L), ++depth);

	ctx->AddPto(id, pto);

	return 0;
}

static int EncodePto(lua_State* L) {
	Context* ctx = (Context*)lua_touserdata(L, 1);

	uint16_t id = (uint16_t)luaL_checkinteger(L, 2);
	Protocol* pto = ctx->GetPto(id);
	if ( !pto ) {
		luaL_error(L, "no such pto:%d", id);
	}

	luaL_checkstack(L, MAX_DEPTH * 2 + 8, NULL);

	Encoder encoder;
	try {
		int depth = 1;
		for ( uint32_t i = 0; i < pto->fields_.size(); i++ ) {
			Field* field = pto->GetField(i);
			lua_getfield(L, 3, field->name_);
			encoder.EncodeOne(L, field, 4, depth);
			lua_pop(L, 1);
		}
	}
	catch ( BadPto& e ) {
		luaL_error(L, e.reason_.c_str());
	}

	lua_pushlstring(L, encoder.ptr_, encoder.offset_);
	return 1;
}

static int DecodePto(lua_State* L) {
	Context* ctx = (Context*)lua_touserdata(L, 1);

	uint16_t id = (uint16_t)luaL_checkinteger(L, 2);
	Protocol* pto = ctx->GetPto(id);
	if ( !pto ) {
		luaL_error(L, "no such pto:%d", id);
	}

	size_t size;
	const char* str = NULL;
	switch ( lua_type(L, 3) ) {
		case LUA_TSTRING:
			str = lua_tolstring(L, 3, &size);
			break;

		case LUA_TLIGHTUSERDATA:
			str = (const char*)lua_touserdata(L, 3);
			size = (size_t)lua_tointeger(L, 4);
			break;

		default:
			luaL_error(L, "decode protocol:%s error,unkown type:%s", pto->name_, lua_typename(L, lua_type(L, 3)));
	}

	Decoder decoder(str, size);

	int depth = 1;
	luaL_checkstack(L, MAX_DEPTH * 2 + 8, NULL);

	lua_createtable(L, 0, pto->fields_.size());
	int top = lua_gettop(L);

	try {
		for ( uint32_t i = 0; i < pto->fields_.size(); i++ ) {
			Field* field = pto->GetField(i);
			decoder.DecodeOne(L, field, top, depth);
		}
	}
	catch ( BadPto& e ) {
		luaL_error(L, e.reason_.c_str());
	}

	if ( decoder.offset_ != decoder.size_ ) {
		luaL_error(L, "decode protocol:%s error", pto->name_);
	}

	return 1;
}

static int Release(lua_State* L) {
	Context* ctx = (Context*)lua_touserdata(L, 1);
	ctx->~Context();
	return 0;
}

static int Create(lua_State* L) {
	void* userdata = lua_newuserdata(L, sizeof(Context));
	Context* ctx = new (userdata)Context();

	lua_pushvalue(L, -1);
	if ( luaL_newmetatable(L, "pto") ) {
		const luaL_Reg meta[] = {
			{"Import", ImportPto},
			{"Encode", EncodePto},
			{"Decode", DecodePto},
			{NULL, NULL},
		};

		luaL_newlib(L, meta);
		lua_setfield(L, -2, "__index");

		lua_pushcfunction(L, Release);
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);

	return 1;
}


int luaopen_ptocxx(lua_State* L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{"Create", Create},
		{NULL, NULL},
	};
	luaL_newlib(L, l);

	lua_pushinteger(L, eTYPE::Bool);
	lua_setfield(L, -2, "BOOL");

	lua_pushinteger(L, eTYPE::Short);
	lua_setfield(L, -2, "SHORT");

	lua_pushinteger(L, eTYPE::Int);
	lua_setfield(L, -2, "INT");

	lua_pushinteger(L, eTYPE::Float);
	lua_setfield(L, -2, "FLOAT");

	lua_pushinteger(L, eTYPE::Double);
	lua_setfield(L, -2, "DOUBLE");

	lua_pushinteger(L, eTYPE::String);
	lua_setfield(L, -2, "STRING");

	lua_pushinteger(L, eTYPE::Pto);
	lua_setfield(L, -2, "PROTOCOL");

	return 1;
}
