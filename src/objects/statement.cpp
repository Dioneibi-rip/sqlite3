const napi_type_tag Statement::TYPE_TAG = RandomTypeTag();

Statement::Statement(const Napi::CallbackInfo& info) :
	Napi::ObjectWrap<Statement>(info),
	db(NULL),
	handle(NULL),
	extras(NULL),
	alive(false),
	locked(false),
	bound(false),
	has_bind_map(false),
	safe_ints(false),
	mode(Data::FLAT),
	returns_data(false) {
	napi_status status = napi_type_tag_object(info.Env(), info.This(), &TYPE_TAG);
	assert(status == napi_ok); ((void)status);
	JS_new(info);
}

Statement::~Statement() {
	if (alive) db->RemoveStatement(this);
	CloseHandles();
	delete extras;
}

// Whenever this is used, db->RemoveStatement must be invoked beforehand.
void Statement::CloseHandles() {
	if (alive) {
		alive = false;
		sqlite3_finalize(handle);
	}
}

// Returns the Statement's bind map (creates it upon first execution).
BindMap& Statement::GetBindMap(Napi::Env env) {
	if (has_bind_map) return extras->bind_map;
	BindMap& bind_map = extras->bind_map;
	int param_count = sqlite3_bind_parameter_count(handle);
	for (int i = 1; i <= param_count; ++i) {
		const char* name = sqlite3_bind_parameter_name(handle, i);
		if (name != NULL) bind_map.Add(env, name + 1, i);
	}
	has_bind_map = true;
	return bind_map;
}

// Returns the Statement's row builder.
RowBuilder& Statement::GetRowBuilder() {
	return extras->row_builder;
}

Statement::Extras::Extras(
	Napi::Env env,
	Napi::Function row_factory,
	Napi::Function array_factory,
	sqlite3_uint64 id
) :
	bind_map(0),
	row_builder(env, row_factory, array_factory),
	id(id) {}


namespace {
	struct AsyncBindValue {
		enum Type { Null, Double, Int64, Text, Blob } type;
		int index;
		double number;
		int64_t integer;
		std::string bytes;
	};

	class AsyncBinder {
	public:
		AsyncBinder(Napi::Env env, sqlite3_stmt* handle, Statement* stmt) :
			env(env), handle(handle), stmt(stmt), param_count(sqlite3_bind_parameter_count(handle)), anon_index(0), success(true), count(0), bound_object(false) {}

		bool Capture(const Napi::CallbackInfo& info, std::vector<AsyncBindValue>& out) {
			for (size_t i = 0; i < info.Length(); ++i) {
				Napi::Value arg = info[i];
				if (arg.IsArray()) {
					CaptureArray(arg.As<Napi::Array>(), out);
					if (!success) break;
					continue;
				}
				if (arg.IsObject() && !arg.IsBuffer()) {
					Napi::Object obj = arg.As<Napi::Object>();
					if (IsPlainObject(obj)) {
						if (bound_object) {
							Fail(ThrowTypeError, "You cannot specify named parameters in two different objects");
							break;
						}
						bound_object = true;
						CaptureObject(obj, out);
						if (!success) break;
						continue;
					} else if (env.IsExceptionPending()) {
						success = false;
						break;
					} else if (stmt->GetBindMap(env).GetSize()) {
						Fail(ThrowTypeError, "Named parameters can only be passed within plain objects");
						break;
					}
				}
				CaptureValue(arg, NextAnonIndex(), out);
				if (!success) break;
				count += 1;
			}
			if (success && count != param_count) {
				if (count < param_count) {
					if (!bound_object && stmt->GetBindMap(env).GetSize()) Fail(ThrowTypeError, "Missing named parameters");
					else Fail(ThrowRangeError, "Too few parameter values were provided");
				} else {
					Fail(ThrowRangeError, "Too many parameter values were provided");
				}
			}
			return success;
		}

	private:
		static Napi::Value GetPrototype(Napi::Env env, Napi::Object obj) {
			napi_value proto;
			if (napi_get_prototype(env, obj, &proto) != napi_ok) return Napi::Value();
			return Napi::Value(env, proto);
		}

		bool IsPlainObject(Napi::Object obj) {
			Napi::Value proto = GetPrototype(env, obj);
			if (proto.IsEmpty()) return false;
			if (proto.IsNull()) return true;
			Napi::Value grandproto = GetPrototype(env, proto.As<Napi::Object>());
			if (grandproto.IsEmpty()) return false;
			return grandproto.IsNull();
		}

		void Fail(Napi::Value (*Throw)(Napi::Env, const char*), const char* message) {
			if (success && Throw) Throw(env, message);
			success = false;
		}

		int NextAnonIndex() {
			while (sqlite3_bind_parameter_name(handle, ++anon_index) != NULL) {}
			return anon_index;
		}

		void CaptureValue(Napi::Value value, int index, std::vector<AsyncBindValue>& out) {
			AsyncBindValue item;
			item.index = index;
			item.number = 0;
			item.integer = 0;
			if (value.IsNumber()) {
				item.type = AsyncBindValue::Double;
				item.number = value.As<Napi::Number>().DoubleValue();
			} else if (value.IsBigInt()) {
				bool lossless;
				item.integer = value.As<Napi::BigInt>().Int64Value(&lossless);
				if (!lossless) return Fail(ThrowRangeError, "The bound string, buffer, or bigint is too big");
				item.type = AsyncBindValue::Int64;
			} else if (value.IsString()) {
				item.type = AsyncBindValue::Text;
				item.bytes = value.As<Napi::String>().Utf8Value();
			} else if (value.IsBuffer()) {
				item.type = AsyncBindValue::Blob;
				Napi::Buffer<char> buffer = value.As<Napi::Buffer<char>>();
				item.bytes.assign(buffer.Data(), buffer.Data() + buffer.Length());
			} else if (value.IsNull() || value.IsUndefined()) {
				item.type = AsyncBindValue::Null;
			} else {
				return Fail(ThrowTypeError, "SQLite3 can only bind numbers, strings, bigints, buffers, and null");
			}
			out.emplace_back(std::move(item));
		}

		void CaptureArray(Napi::Array arr, std::vector<AsyncBindValue>& out) {
			uint32_t length = arr.Length();
			if (length > INT_MAX) return Fail(ThrowRangeError, "Too many parameter values were provided");
			int len = static_cast<int>(length);
			for (int i = 0; i < len; ++i) {
				Napi::Value value = SafeGetElement(env, arr, static_cast<uint32_t>(i));
				if (value.IsEmpty()) { success = false; return; }
				CaptureValue(value, NextAnonIndex(), out);
				if (!success) return;
			}
			count += len;
		}

		void CaptureObject(Napi::Object obj, std::vector<AsyncBindValue>& out) {
			BindMap& bind_map = stmt->GetBindMap(env);
			BindMap::Pair* pairs = bind_map.GetPairs();
			int len = bind_map.GetSize();
			for (int i = 0; i < len; ++i) {
				Napi::String key = pairs[i].GetName(env);
				bool has_property;
				if (!SafeHasOwnProperty(env, obj, key, &has_property)) { success = false; return; }
				if (!has_property) {
					std::string param_name = key.Utf8Value();
					Fail(ThrowRangeError, (std::string("Missing named parameter \"") + param_name + "\"").c_str());
					return;
				}
				Napi::Value value = SafeGet(env, obj, key);
				if (value.IsEmpty()) { success = false; return; }
				CaptureValue(value, pairs[i].GetIndex(), out);
				if (!success) return;
			}
			count += len;
		}

		Napi::Env env;
		sqlite3_stmt* handle;
		Statement* stmt;
		int param_count;
		int anon_index;
		bool success;
		int count;
		bool bound_object;
	};

	struct ResultValue {
		enum Type { Null, Integer, Float, Text, Blob } type;
		sqlite3_int64 integer;
		double number;
		std::string text;
		std::vector<uint8_t> blob;

		ResultValue() : type(Null), integer(0), number(0) {}
	};

	struct ResultRow {
		std::vector<ResultValue> values;
	};

	struct ResultColumn {
		std::string name;
		std::string table;
	};

	static ResultValue CaptureColumnValue(sqlite3_stmt* handle, int column) {
		ResultValue value;
		switch (sqlite3_column_type(handle, column)) {
			case SQLITE_INTEGER:
				value.type = ResultValue::Integer;
				value.integer = sqlite3_column_int64(handle, column);
				break;
			case SQLITE_FLOAT:
				value.type = ResultValue::Float;
				value.number = sqlite3_column_double(handle, column);
				break;
			case SQLITE_TEXT: {
				value.type = ResultValue::Text;
				const char* data = reinterpret_cast<const char*>(sqlite3_column_text(handle, column));
				int size = sqlite3_column_bytes(handle, column);
				if (data != NULL && size > 0) value.text.assign(data, data + size);
				break;
			}
			case SQLITE_BLOB: {
				value.type = ResultValue::Blob;
				const uint8_t* data = static_cast<const uint8_t*>(sqlite3_column_blob(handle, column));
				int size = sqlite3_column_bytes(handle, column);
				if (data != NULL && size > 0) value.blob.assign(data, data + size);
				break;
			}
			default:
				value.type = ResultValue::Null;
				break;
		}
		return value;
	}

	static Napi::Value ResultValueToJS(Napi::Env env, const ResultValue& value, bool safe_ints) {
		switch (value.type) {
			case ResultValue::Integer:
				if (safe_ints) return Napi::BigInt::New(env, (int64_t)value.integer);
				return Napi::Number::New(env, (double)value.integer);
			case ResultValue::Float:
				return Napi::Number::New(env, value.number);
			case ResultValue::Text:
				return StringFromUtf8(env, value.text.c_str(), value.text.length());
			case ResultValue::Blob:
				return Napi::Buffer<uint8_t>::Copy(env, value.blob.data(), value.blob.size());
			default:
				return env.Null();
		}
	}

	class AsyncReaderWorker : public QueuedAsyncWorker {
	public:
		AsyncReaderWorker(Napi::Env env, Database* db, Napi::Object owner, Statement* stmt, std::vector<AsyncBindValue> values, bool bound, bool single)
			: QueuedAsyncWorker(env, db, owner), stmt(stmt), values(std::move(values)), bound(bound), single(single), safe_ints(stmt->IsSafeIntegers()), mode(stmt->GetMode()), column_count(0), status(SQLITE_OK), code(SQLITE_OK) {}

		void Execute() override {
			sqlite3_stmt* handle = stmt->GetHandle();
			sqlite3* db_handle = db->GetHandle();
			if (!bound) {
				for (const AsyncBindValue& value : values) {
					status = Bind(handle, value);
					if (status != SQLITE_OK) break;
				}
			}
			if (status == SQLITE_OK) {
				column_count = sqlite3_column_count(handle);
				CaptureColumns(handle);
				while ((status = sqlite3_step(handle)) == SQLITE_ROW) {
					ResultRow row;
					row.values.reserve(column_count);
					for (int i = 0; i < column_count; ++i) row.values.emplace_back(CaptureColumnValue(handle, i));
					rows.emplace_back(std::move(row));
					if (single) break;
				}
				if (status == SQLITE_ROW || status == SQLITE_DONE) status = sqlite3_reset(handle);
				else sqlite3_reset(handle);
			}
			if (status != SQLITE_OK) {
				code = sqlite3_extended_errcode(db_handle);
				message = sqlite3_errmsg(db_handle);
				SetError(message);
			}
			if (!bound) sqlite3_clear_bindings(handle);
		}

		void OnOK() override {
			if (single) {
				deferred.Resolve(rows.empty() ? Env().Undefined() : RowToJS(rows[0]));
			} else {
				Napi::Array result = Napi::Array::New(Env(), rows.size());
				for (size_t i = 0; i < rows.size(); ++i) result.Set(i, RowToJS(rows[i]));
				deferred.Resolve(result);
			}
			FinishQueue();
		}

		void OnError(const Napi::Error& error) override {
			Napi::Object err = error.Value();
			err.Set("code", db->GetAddon()->cs.Code(Env(), code));
			deferred.Reject(err);
			FinishQueue();
		}

	private:
		int Bind(sqlite3_stmt* handle, const AsyncBindValue& value) {
			switch (value.type) {
				case AsyncBindValue::Null: return sqlite3_bind_null(handle, value.index);
				case AsyncBindValue::Double: return sqlite3_bind_double(handle, value.index, value.number);
				case AsyncBindValue::Int64: return sqlite3_bind_int64(handle, value.index, value.integer);
				case AsyncBindValue::Text: return sqlite3_bind_text(handle, value.index, value.bytes.c_str(), value.bytes.length(), SQLITE_TRANSIENT);
				case AsyncBindValue::Blob: return sqlite3_bind_blob(handle, value.index, value.bytes.data(), value.bytes.length(), SQLITE_TRANSIENT);
			}
			return SQLITE_MISUSE;
		}

		void CaptureColumns(sqlite3_stmt* handle) {
			columns.clear();
			columns.reserve(column_count);
			for (int i = 0; i < column_count; ++i) {
				const char* name = sqlite3_column_name(handle, i);
				const char* table = sqlite3_column_table_name(handle, i);
				ResultColumn column;
				column.name = name == NULL ? "" : name;
				column.table = table == NULL ? "$" : table;
				columns.emplace_back(std::move(column));
			}
		}

		Napi::Value RowToJS(const ResultRow& row) {
			Napi::Env env = Env();
			if (mode == Data::PLUCK) return ResultValueToJS(env, row.values[0], safe_ints);
			if (mode == Data::RAW) {
				Napi::Array array = Napi::Array::New(env, row.values.size());
				for (size_t i = 0; i < row.values.size(); ++i) array.Set(i, ResultValueToJS(env, row.values[i], safe_ints));
				return array;
			}
			Napi::Object object = Napi::Object::New(env);
			for (size_t i = 0; i < row.values.size(); ++i) {
				Napi::String key = Napi::String::New(env, columns[i].name);
				Napi::Value value = ResultValueToJS(env, row.values[i], safe_ints);
				if (mode == Data::EXPAND) {
					Napi::String table = Napi::String::New(env, columns[i].table);
					if (object.HasOwnProperty(table)) object.Get(table).As<Napi::Object>().Set(key, value);
					else {
						Napi::Object nested = Napi::Object::New(env);
						object.Set(table, nested);
						nested.Set(key, value);
					}
				} else {
					object.Set(key, value);
				}
			}
			return object;
		}

		Statement* stmt;
		std::vector<AsyncBindValue> values;
		bool bound;
		bool single;
		bool safe_ints;
		char mode;
		int column_count;
		std::vector<ResultColumn> columns;
		std::vector<ResultRow> rows;
		int status;
		int code;
		std::string message;
	};

	class RunAsyncWorker : public QueuedAsyncWorker {
	public:
		RunAsyncWorker(Napi::Env env, Database* db, Napi::Object owner, Statement* stmt, std::vector<AsyncBindValue> values, bool bound)
			: QueuedAsyncWorker(env, db, owner), stmt(stmt), values(std::move(values)), bound(bound), changes(0), id(0), safe_ints(stmt->IsSafeIntegers()), status(SQLITE_OK), code(SQLITE_OK) {}

		void Execute() override {
			sqlite3_stmt* handle = stmt->GetHandle();
			sqlite3* db_handle = db->GetHandle();
			int total_changes_before = sqlite3_total_changes(db_handle);
			if (!bound) {
				for (const AsyncBindValue& value : values) {
					status = Bind(handle, value);
					if (status != SQLITE_OK) break;
				}
			}
			if (status == SQLITE_OK) {
				sqlite3_step(handle);
				status = sqlite3_reset(handle);
			}
			if (status == SQLITE_OK) {
				changes = sqlite3_total_changes(db_handle) == total_changes_before ? 0 : sqlite3_changes(db_handle);
				id = sqlite3_last_insert_rowid(db_handle);
			} else {
				code = sqlite3_extended_errcode(db_handle);
				message = sqlite3_errmsg(db_handle);
				SetError(message);
			}
			if (!bound) sqlite3_clear_bindings(handle);
		}

		void OnOK() override {
			Napi::Object result = Napi::Object::New(Env());
			result.Set(db->GetAddon()->cs.changes.Value(), Napi::Number::New(Env(), changes));
			if (safe_ints) result.Set(db->GetAddon()->cs.lastInsertRowid.Value(), Napi::BigInt::New(Env(), (int64_t)id));
			else result.Set(db->GetAddon()->cs.lastInsertRowid.Value(), Napi::Number::New(Env(), (double)id));
			deferred.Resolve(result);
			FinishQueue();
		}

		void OnError(const Napi::Error& error) override {
			Napi::Object err = error.Value();
			err.Set("code", db->GetAddon()->cs.Code(Env(), code));
			deferred.Reject(err);
			FinishQueue();
		}

	private:
		int Bind(sqlite3_stmt* handle, const AsyncBindValue& value) {
			switch (value.type) {
				case AsyncBindValue::Null: return sqlite3_bind_null(handle, value.index);
				case AsyncBindValue::Double: return sqlite3_bind_double(handle, value.index, value.number);
				case AsyncBindValue::Int64: return sqlite3_bind_int64(handle, value.index, value.integer);
				case AsyncBindValue::Text: return sqlite3_bind_text(handle, value.index, value.bytes.c_str(), value.bytes.length(), SQLITE_TRANSIENT);
				case AsyncBindValue::Blob: return sqlite3_bind_blob(handle, value.index, value.bytes.data(), value.bytes.length(), SQLITE_TRANSIENT);
			}
			return SQLITE_MISUSE;
		}

		Statement* stmt;
		std::vector<AsyncBindValue> values;
		bool bound;
		int changes;
		sqlite3_int64 id;
		bool safe_ints;
		int status;
		int code;
		std::string message;
	};
}


namespace {
class StatementAsyncIterator : public Napi::ObjectWrap<StatementAsyncIterator> {
public:
	explicit StatementAsyncIterator(const Napi::CallbackInfo& info) :
		Napi::ObjectWrap<StatementAsyncIterator>(info),
		stmt(NULL),
		handle(NULL),
		db(NULL),
		db_state(NULL),
		bound(false),
		safe_ints(false),
		mode(Data::FLAT),
		alive(false),
		pending(false) {
		napi_status status = napi_type_tag_object(info.Env(), info.This(), &TYPE_TAG);
		assert(status == napi_ok); ((void)status);
		JS_new(info);
	}

	~StatementAsyncIterator() {}

	static const napi_type_tag TYPE_TAG;
	static INIT(Init) {
		return DefineClass(env, "StatementAsyncIterator", {
			PrototypeMethod<StatementAsyncIterator, &StatementAsyncIterator::JS_next>("next", addon),
			PrototypeMethod<StatementAsyncIterator, &StatementAsyncIterator::JS_return>("return", addon),
			PrototypeSymbolMethod<StatementAsyncIterator, &StatementAsyncIterator::JS_symbolAsyncIterator>(Napi::Symbol::WellKnown(env, "asyncIterator"), addon),
		}, addon);
	}

	Napi::Object DoneRecord(Napi::Env env) {
		return NewRecord(env, env.Undefined(), true);
	}

	Napi::Object NewRecord(Napi::Env env, Napi::Value value, bool done) {
		Addon* addon = db_state->addon;
		napi_property_descriptor properties[2] = {};
		properties[0].name = addon->cs.value.Value();
		properties[0].value = value;
		properties[0].attributes = DEFAULT_ATTRIBUTES;
		properties[1].name = addon->cs.done.Value();
		properties[1].value = Napi::Boolean::New(env, done);
		properties[1].attributes = DEFAULT_ATTRIBUTES;
		napi_value record;
		napi_status status = napi_create_object(env, &record);
		assert(status == napi_ok);
		status = napi_define_properties(env, record, 2, properties);
		assert(status == napi_ok); ((void)status);
		return Napi::Object(env, record);
	}

	void Cleanup() {
		if (!alive) return;
		alive = false;
		stmt->SetLocked(false);
		db_state->iterators -= 1;
		sqlite3_reset(handle);
		if (!bound) sqlite3_clear_bindings(handle);
	}

private:
	class NextWorker : public QueuedAsyncWorker {
	public:
		NextWorker(Napi::Env env, Database* db, Napi::Object owner, StatementAsyncIterator* iter)
			: QueuedAsyncWorker(env, db, owner), iter(iter), status(SQLITE_OK), code(SQLITE_OK), has_row(false), done(false), safe_ints(iter->safe_ints), mode(iter->mode), column_count(0) {}

		void Execute() override {
			sqlite3_stmt* handle = iter->handle;
			column_count = sqlite3_column_count(handle);
			CaptureColumns(handle);
			status = sqlite3_step(handle);
			if (status == SQLITE_ROW) {
				has_row = true;
				row.values.reserve(column_count);
				for (int i = 0; i < column_count; ++i) row.values.emplace_back(CaptureColumnValue(handle, i));
			} else if (status == SQLITE_DONE) {
				done = true;
			} else {
				code = sqlite3_extended_errcode(db->GetHandle());
				message = sqlite3_errmsg(db->GetHandle());
				SetError(message);
			}
		}

		void OnOK() override {
			iter->pending = false;
			if (done) {
				iter->Cleanup();
				deferred.Resolve(iter->DoneRecord(Env()));
			} else {
				deferred.Resolve(iter->NewRecord(Env(), RowToJS(row), false));
			}
			FinishQueue();
		}

		void OnError(const Napi::Error& error) override {
			iter->pending = false;
			iter->Cleanup();
			Napi::Object err = error.Value();
			err.Set("code", db->GetAddon()->cs.Code(Env(), code));
			deferred.Reject(err);
			FinishQueue();
		}

	private:
		void CaptureColumns(sqlite3_stmt* handle) {
			columns.clear();
			columns.reserve(column_count);
			for (int i = 0; i < column_count; ++i) {
				const char* name = sqlite3_column_name(handle, i);
				const char* table = sqlite3_column_table_name(handle, i);
				ResultColumn column;
				column.name = name == NULL ? "" : name;
				column.table = table == NULL ? "$" : table;
				columns.emplace_back(std::move(column));
			}
		}

		Napi::Value RowToJS(const ResultRow& row) {
			Napi::Env env = Env();
			if (mode == Data::PLUCK) return ResultValueToJS(env, row.values[0], safe_ints);
			if (mode == Data::RAW) {
				Napi::Array array = Napi::Array::New(env, row.values.size());
				for (size_t i = 0; i < row.values.size(); ++i) array.Set(i, ResultValueToJS(env, row.values[i], safe_ints));
				return array;
			}
			Napi::Object object = Napi::Object::New(env);
			for (size_t i = 0; i < row.values.size(); ++i) {
				Napi::String key = Napi::String::New(env, columns[i].name);
				Napi::Value value = ResultValueToJS(env, row.values[i], safe_ints);
				if (mode == Data::EXPAND) {
					Napi::String table = Napi::String::New(env, columns[i].table);
					if (object.HasOwnProperty(table)) object.Get(table).As<Napi::Object>().Set(key, value);
					else {
						Napi::Object nested = Napi::Object::New(env);
						object.Set(table, nested);
						nested.Set(key, value);
					}
				} else {
					object.Set(key, value);
				}
			}
			return object;
		}

		StatementAsyncIterator* iter;
		int status;
		int code;
		bool has_row;
		bool done;
		bool safe_ints;
		char mode;
		int column_count;
		ResultRow row;
		std::vector<ResultColumn> columns;
		std::string message;
	};

	NODE_METHOD(JS_new) {
		UseAddon;
		if (!addon->privileged_info) return ThrowTypeError(info.Env(), "Disabled constructor");
		assert(info.IsConstructCall());

		{
			const Napi::CallbackInfo& pinfo = *addon->privileged_info;
			Statement* stmt = ::Unwrap<Statement>(pinfo.This());
			if (!stmt->ReturnsData()) return ThrowTypeError(pinfo.Env(), "This statement does not return data. Use run() instead");
			sqlite3_stmt* handle = stmt->GetHandle();
			Database* db = stmt->GetDatabase();
			REQUIRE_DATABASE_OPEN(db->GetState());
			REQUIRE_DATABASE_NOT_BUSY(db->GetState());
			if (stmt->IsLocked()) return ThrowTypeError(pinfo.Env(), "This statement is busy executing a query");
			if (db->GetState()->iterators == USHRT_MAX) return ThrowRangeError(pinfo.Env(), "Too many active database iterators");
			if (db->GetState()->has_logger) return ThrowTypeError(pinfo.Env(), "iterateAsync() cannot be used while verbose logging is enabled");
			const bool bound = stmt->IsBound();
			if (!bound) {
				Binder binder(handle);
				if (!binder.Bind(pinfo, pinfo.Length(), stmt)) {
					sqlite3_clear_bindings(handle);
					return pinfo.Env().Undefined();
				}
			} else if (pinfo.Length() > 0) {
				return ThrowTypeError(pinfo.Env(), "This statement already has bound parameters");
			}
			this->stmt = stmt;
			this->handle = handle;
			this->db = db;
			this->db_state = db->GetState();
			this->bound = bound;
			this->safe_ints = stmt->IsSafeIntegers();
			this->mode = stmt->GetMode();
			this->alive = true;
			assert(stmt != NULL);
			assert(handle != NULL);
			assert(stmt->IsBound() == bound);
			assert(stmt->IsLocked() == false);
			assert(db_state->iterators < USHRT_MAX);
			stmt->SetLocked(true);
			db_state->iterators += 1;
		}

		UseIsolate;
		SetFrozen(env, info.This().As<Napi::Object>(), addon->cs.statement, addon->privileged_info->This());
		return info.This();
	}

	static NODE_METHOD(JS_next) {
		StatementAsyncIterator* iter = ::Unwrap<StatementAsyncIterator>(info.This());
		Napi::Env env = info.Env();
		if (!iter->alive) {
			Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
			deferred.Resolve(iter->DoneRecord(env));
			return deferred.Promise();
		}
		if (iter->pending) return ThrowTypeError(env, "This async iterator already has a pending next() call");
		if (iter->db_state->busy) return ThrowTypeError(env, "This database connection is busy executing a query");
		iter->pending = true;
		NextWorker* worker = new NextWorker(env, iter->db, info.This().As<Napi::Object>(), iter);
		Napi::Promise promise = worker->Promise();
		iter->db->EnqueueAsync(worker);
		return promise;
	}

	static NODE_METHOD(JS_return) {
		StatementAsyncIterator* iter = ::Unwrap<StatementAsyncIterator>(info.This());
		Napi::Env env = info.Env();
		if (iter->pending) return ThrowTypeError(env, "This async iterator already has a pending next() call");
		if (iter->alive) iter->Cleanup();
		Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
		deferred.Resolve(iter->DoneRecord(env));
		return deferred.Promise();
	}

	static NODE_METHOD(JS_symbolAsyncIterator) {
		return info.This();
	}

	Statement* stmt;
	sqlite3_stmt* handle;
	Database* db;
	Database::State* db_state;
	bool bound;
	bool safe_ints;
	char mode;
	bool alive;
	bool pending;
};

const napi_type_tag StatementAsyncIterator::TYPE_TAG = RandomTypeTag();
}

INIT(Statement::Init) {
	return DefineClass(env, "Statement", {
		PrototypeMethod<Statement, &Statement::JS_run>("run", addon),
		PrototypeMethod<Statement, &Statement::JS_runAsync>("runAsync", addon),
		PrototypeMethod<Statement, &Statement::JS_get>("get", addon),
		PrototypeMethod<Statement, &Statement::JS_getAsync>("getAsync", addon),
		PrototypeMethod<Statement, &Statement::JS_all>("all", addon),
		PrototypeMethod<Statement, &Statement::JS_allAsync>("allAsync", addon),
		PrototypeMethod<Statement, &Statement::JS_iterate>("iterate", addon),
		PrototypeMethod<Statement, &Statement::JS_iterateAsync>("iterateAsync", addon),
		PrototypeMethod<Statement, &Statement::JS_bind>("bind", addon),
		PrototypeMethod<Statement, &Statement::JS_pluck>("pluck", addon),
		PrototypeMethod<Statement, &Statement::JS_expand>("expand", addon),
		PrototypeMethod<Statement, &Statement::JS_raw>("raw", addon),
		PrototypeMethod<Statement, &Statement::JS_safeIntegers>("safeIntegers", addon),
		PrototypeMethod<Statement, &Statement::JS_columns>("columns", addon),
		PrototypeMethod<Statement, &Statement::JS_toString>("toString", addon),
	}, addon);
}

NODE_METHOD(Statement::JS_new) {
	UseAddon;
	if (!addon->privileged_info) {
		return ThrowTypeError(info.Env(), "Statements can only be constructed by the db.prepare() method");
	}
	assert(info.IsConstructCall());
	const Napi::CallbackInfo& pinfo = *addon->privileged_info;
	Database* db = ::Unwrap<Database>(pinfo.This());
	REQUIRE_DATABASE_OPEN(db->GetState());
	REQUIRE_DATABASE_NOT_BUSY(db->GetState());

	Napi::String source = pinfo[0].As<Napi::String>();
	Napi::Object database = pinfo[1].As<Napi::Object>();
	bool pragmaMode = pinfo[2].As<Napi::Boolean>().Value();
	bool explainMode = pinfo[3].As<Napi::Boolean>().Value();
	int flags = SQLITE_PREPARE_PERSISTENT;

	if (pragmaMode) {
		REQUIRE_DATABASE_NO_ITERATORS_UNLESS_UNSAFE(db->GetState());
		flags = 0;
	}
	if (explainMode) {
		flags = 0;
	}

	UseIsolate;
	std::string utf8 = source.Utf8Value();
	sqlite3_stmt* handle;
	const char* tail;

	if (sqlite3_prepare_v3(db->GetHandle(), utf8.c_str(), utf8.length() + 1, flags, &handle, &tail) != SQLITE_OK) {
		db->ThrowDatabaseError(env);
		return env.Undefined();
	}
	if (handle == NULL) {
		return ThrowRangeError(env, "The supplied SQL string contains no statements");
	}
	// https://github.com/WiseLibs/better-sqlite3/issues/975#issuecomment-1520934678
	for (char c; (c = *tail); ) {
		if (IS_SKIPPED(c)) {
			++tail;
			continue;
		}
		if (c == '/' && tail[1] == '*') {
			tail += 2;
			for (char c; (c = *tail); ++tail) {
				if (c == '*' && tail[1] == '/') {
					tail += 2;
					break;
				}
			}
		} else if (c == '-' && tail[1] == '-') {
			tail += 2;
			for (char c; (c = *tail); ++tail) {
				if (c == '\n') {
					++tail;
					break;
				}
			}
		} else {
			sqlite3_finalize(handle);
			return ThrowRangeError(env, "The supplied SQL string contains more than one statement");
		}
	}

	bool returns_data = sqlite3_column_count(handle) >= 1 || pragmaMode;
	this->db = db;
	this->handle = handle;
	this->extras = new Extras(env, addon->RowFactory.Value(), addon->ArrayFactory.Value(), addon->NextId());
	this->bound = explainMode;
	this->safe_ints = db->GetState()->safe_ints;
	this->returns_data = returns_data;
	this->alive = true;
	assert(db->GetState()->open);
	assert(!db->GetState()->busy);
	db->AddStatement(this);

	Napi::Object _this = info.This().As<Napi::Object>();
	SetFrozen(env, _this, addon->cs.reader, Napi::Boolean::New(env, returns_data));
	SetFrozen(env, _this, addon->cs.readonly, Napi::Boolean::New(env, sqlite3_stmt_readonly(handle) != 0));
	SetFrozen(env, _this, addon->cs.source, source);
	SetFrozen(env, _this, addon->cs.database, database);
	SetInstanceGetter<Statement, &Statement::JS_busy>(_this, "busy", addon);

	return info.This();
}

NODE_METHOD(Statement::JS_run) {
	STATEMENT_START(ALLOW_ANY_STATEMENT, DOES_MUTATE);
	sqlite3* db_handle = db->GetHandle();
	int total_changes_before = sqlite3_total_changes(db_handle);

	sqlite3_step(handle);
	if (sqlite3_reset(handle) == SQLITE_OK) {
		int changes = sqlite3_total_changes(db_handle) == total_changes_before ? 0 : sqlite3_changes(db_handle);
		sqlite3_int64 id = sqlite3_last_insert_rowid(db_handle);
		Addon* addon = db->GetAddon();

		napi_property_descriptor properties[2] = {};
		properties[0].name = addon->cs.changes.Value();
		properties[0].value = Napi::Number::New(env, changes);
		properties[0].attributes = DEFAULT_ATTRIBUTES;
		properties[1].name = addon->cs.lastInsertRowid.Value();
		if (stmt->safe_ints) {
			properties[1].value = Napi::BigInt::New(env, (int64_t)id);
		} else {
			properties[1].value = Napi::Number::New(env, (double)id);
		}
		properties[1].attributes = DEFAULT_ATTRIBUTES;

		napi_value result;
		napi_status status = napi_create_object(env, &result);
		assert(status == napi_ok);
		status = napi_define_properties(env, result, 2, properties);
		assert(status == napi_ok); ((void)status);
		STATEMENT_RETURN(Napi::Object(env, result));
	}
	STATEMENT_THROW();
}

NODE_METHOD(Statement::JS_get) {
	STATEMENT_START(REQUIRE_STATEMENT_RETURNS_DATA, DOES_NOT_MUTATE);
	int status = sqlite3_step(handle);
	if (status == SQLITE_ROW) {
		Napi::Value result = Data::GetRowJS(env, stmt, handle, stmt->safe_ints, stmt->mode);
		sqlite3_reset(handle);
		STATEMENT_RETURN(result);
	} else if (status == SQLITE_DONE) {
		sqlite3_reset(handle);
		STATEMENT_RETURN(env.Undefined());
	}
	sqlite3_reset(handle);
	STATEMENT_THROW();
}


NODE_METHOD(Statement::JS_getAsync) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	REQUIRE_STATEMENT_RETURNS_DATA();
	sqlite3_stmt* handle = stmt->handle;
	Database* db = stmt->db;
	REQUIRE_DATABASE_OPEN(db->GetState());
	if (db->GetState()->busy) return ThrowTypeError(info.Env(), "This database connection is busy executing a query");
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	if (db->GetState()->has_logger) return ThrowTypeError(info.Env(), "getAsync() cannot be used while verbose logging is enabled");
	const bool bound = stmt->bound;
	std::vector<AsyncBindValue> values;
	if (!bound) {
		AsyncBinder binder(info.Env(), handle, stmt);
		if (!binder.Capture(info, values)) return info.Env().Undefined();
	} else if (info.Length() > 0) {
		return ThrowTypeError(info.Env(), "This statement already has bound parameters");
	}

	AsyncReaderWorker* worker = new AsyncReaderWorker(info.Env(), db, info.This().As<Napi::Object>(), stmt, std::move(values), bound, true);
	Napi::Promise promise = worker->Promise();
	db->EnqueueAsync(worker);
	return promise;
}

NODE_METHOD(Statement::JS_all) {
	STATEMENT_START(REQUIRE_STATEMENT_RETURNS_DATA, DOES_NOT_MUTATE);
	const bool safe_ints = stmt->safe_ints;
	const char mode = stmt->mode;

	std::vector<napi_value> rows;
	rows.reserve(8);

	while (sqlite3_step(handle) == SQLITE_ROW) {
		rows.emplace_back(Data::GetRowJS(env, stmt, handle, safe_ints, mode));
	}

	if (sqlite3_reset(handle) == SQLITE_OK) {
		if (rows.size() > 0xffffffff) {
			ThrowRangeError(env, "Array overflow (too many rows returned)");
			db->GetState()->was_js_error = true;
		} else {
			Addon* addon = db->GetAddon();
			assert(!addon->ArrayFactory.IsEmpty());
			assert(!addon->ArrayAppender.IsEmpty());
			static const size_t batch_size = 1024;
			size_t first_batch_size = std::min(rows.size(), batch_size);
			Napi::Value result = SafeCall(env, addon->ArrayFactory.Value(), env.Undefined(), first_batch_size, rows.data());
			if (!env.IsExceptionPending()) {
				napi_value args[batch_size + 1];
				args[0] = result;
				for (size_t offset = first_batch_size; offset < rows.size(); offset += batch_size) {
					size_t count = std::min(rows.size() - offset, batch_size);
					std::copy_n(rows.data() + offset, count, args + 1);
					SafeCall(env, addon->ArrayAppender.Value(), env.Undefined(), count + 1, args);
					if (env.IsExceptionPending()) break;
				}
			}
			if (env.IsExceptionPending()) {
				db->GetState()->was_js_error = true;
			} else {
				STATEMENT_RETURN(result);
			}
		}
	}
	STATEMENT_THROW();
}


NODE_METHOD(Statement::JS_allAsync) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	REQUIRE_STATEMENT_RETURNS_DATA();
	sqlite3_stmt* handle = stmt->handle;
	Database* db = stmt->db;
	REQUIRE_DATABASE_OPEN(db->GetState());
	if (db->GetState()->busy) return ThrowTypeError(info.Env(), "This database connection is busy executing a query");
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	if (db->GetState()->has_logger) return ThrowTypeError(info.Env(), "allAsync() cannot be used while verbose logging is enabled");
	const bool bound = stmt->bound;
	std::vector<AsyncBindValue> values;
	if (!bound) {
		AsyncBinder binder(info.Env(), handle, stmt);
		if (!binder.Capture(info, values)) return info.Env().Undefined();
	} else if (info.Length() > 0) {
		return ThrowTypeError(info.Env(), "This statement already has bound parameters");
	}

	AsyncReaderWorker* worker = new AsyncReaderWorker(info.Env(), db, info.This().As<Napi::Object>(), stmt, std::move(values), bound, false);
	Napi::Promise promise = worker->Promise();
	db->EnqueueAsync(worker);
	return promise;
}

NODE_METHOD(Statement::JS_iterate) {
	UseAddon;
	UseIsolate;
	Napi::Function c = addon->StatementIterator.Value();
	addon->privileged_info = &info;
	Napi::Object iterator = SafeConstruct(env, c);
	addon->privileged_info = NULL;
	if (env.IsExceptionPending()) return env.Undefined();
	return iterator;
}

NODE_METHOD(Statement::JS_iterateAsync) {
	UseAddon;
	UseIsolate;
	Napi::Function c = addon->StatementAsyncIterator.Value();
	addon->privileged_info = &info;
	Napi::Object iterator = SafeConstruct(env, c);
	addon->privileged_info = NULL;
	if (env.IsExceptionPending()) return env.Undefined();
	return iterator;
}


NODE_METHOD(Statement::JS_runAsync) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	ALLOW_ANY_STATEMENT();
	sqlite3_stmt* handle = stmt->handle;
	Database* db = stmt->db;
	REQUIRE_DATABASE_OPEN(db->GetState());
	if (db->GetState()->busy) return ThrowTypeError(info.Env(), "This database connection is busy executing a query");
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	REQUIRE_DATABASE_NO_ITERATORS_UNLESS_UNSAFE(db->GetState());
	if (db->GetState()->has_logger) return ThrowTypeError(info.Env(), "runAsync() cannot be used while verbose logging is enabled");
	const bool bound = stmt->bound;
	std::vector<AsyncBindValue> values;
	if (!bound) {
		AsyncBinder binder(info.Env(), handle, stmt);
		if (!binder.Capture(info, values)) return info.Env().Undefined();
	} else if (info.Length() > 0) {
		return ThrowTypeError(info.Env(), "This statement already has bound parameters");
	}

	RunAsyncWorker* worker = new RunAsyncWorker(info.Env(), db, info.This().As<Napi::Object>(), stmt, std::move(values), bound);
	Napi::Promise promise = worker->Promise();
	db->EnqueueAsync(worker);
	return promise;
}

NODE_METHOD(Statement::JS_bind) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	if (stmt->bound) return ThrowTypeError(info.Env(), "The bind() method can only be invoked once per statement object");
	REQUIRE_DATABASE_OPEN(stmt->db->GetState());
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	STATEMENT_BIND(stmt->handle);
	stmt->bound = true;
	return info.This();
}

NODE_METHOD(Statement::JS_pluck) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	if (!stmt->returns_data) return ThrowTypeError(info.Env(), "The pluck() method is only for statements that return data");
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	bool use = true;
	if (info.Length() != 0) { REQUIRE_ARGUMENT_BOOLEAN(first, use); }
	stmt->mode = use ? Data::PLUCK : stmt->mode == Data::PLUCK ? Data::FLAT : stmt->mode;
	return info.This();
}

NODE_METHOD(Statement::JS_expand) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	if (!stmt->returns_data) return ThrowTypeError(info.Env(), "The expand() method is only for statements that return data");
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	bool use = true;
	if (info.Length() != 0) { REQUIRE_ARGUMENT_BOOLEAN(first, use); }
	stmt->mode = use ? Data::EXPAND : stmt->mode == Data::EXPAND ? Data::FLAT : stmt->mode;
	return info.This();
}

NODE_METHOD(Statement::JS_raw) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	if (!stmt->returns_data) return ThrowTypeError(info.Env(), "The raw() method is only for statements that return data");
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	bool use = true;
	if (info.Length() != 0) { REQUIRE_ARGUMENT_BOOLEAN(first, use); }
	stmt->mode = use ? Data::RAW : stmt->mode == Data::RAW ? Data::FLAT : stmt->mode;
	return info.This();
}

NODE_METHOD(Statement::JS_safeIntegers) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	REQUIRE_STATEMENT_NOT_LOCKED(stmt);
	if (info.Length() == 0) stmt->safe_ints = true;
	else { REQUIRE_ARGUMENT_BOOLEAN(first, stmt->safe_ints); }
	return info.This();
}

NODE_METHOD(Statement::JS_columns) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	if (!stmt->returns_data) return ThrowTypeError(info.Env(), "The columns() method is only for statements that return data");
	REQUIRE_DATABASE_OPEN(stmt->db->GetState());
	REQUIRE_DATABASE_NOT_BUSY(stmt->db->GetState());
	Addon* addon = stmt->db->GetAddon();
	UseIsolate;

	int column_count = sqlite3_column_count(stmt->handle);
	Napi::Array columns = Napi::Array::New(env, column_count);

	Napi::String name = addon->cs.name.Value();
	Napi::String columnName = addon->cs.column.Value();
	Napi::String tableName = addon->cs.table.Value();
	Napi::String databaseName = addon->cs.database.Value();
	Napi::String typeName = addon->cs.type.Value();

	for (int i = 0; i < column_count; ++i) {
		Napi::Object column = Napi::Object::New(env);

		column.Set(name,
			InternalizedFromUtf8OrNull(env, sqlite3_column_name(stmt->handle, i), -1)
		);
		column.Set(columnName,
			InternalizedFromUtf8OrNull(env, sqlite3_column_origin_name(stmt->handle, i), -1)
		);
		column.Set(tableName,
			InternalizedFromUtf8OrNull(env, sqlite3_column_table_name(stmt->handle, i), -1)
		);
		column.Set(databaseName,
			InternalizedFromUtf8OrNull(env, sqlite3_column_database_name(stmt->handle, i), -1)
		);
		column.Set(typeName,
			InternalizedFromUtf8OrNull(env, sqlite3_column_decltype(stmt->handle, i), -1)
		);

		columns.Set(i, column);
	}

	return columns;
}

NODE_METHOD(Statement::JS_toString) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	Addon* addon = stmt->db->GetAddon();

	char* expanded = stmt->alive && stmt->bound ? sqlite3_expanded_sql(stmt->handle) : NULL;
	if (expanded != NULL) {
		Napi::Value ret = StringFromUtf8(info.Env(), expanded, -1);
		sqlite3_free(expanded);
		return ret;
	}

	return info.This().As<Napi::Object>()
		.Get(addon->cs.source.Value())
		.As<Napi::String>();
}

NODE_GETTER(Statement::JS_busy) {
	Statement* stmt = ::Unwrap<Statement>(info.This());
	return Napi::Boolean::New(info.Env(), stmt->alive && stmt->locked);
}
