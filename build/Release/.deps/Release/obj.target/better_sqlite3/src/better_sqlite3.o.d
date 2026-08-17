cmd_Release/obj.target/better_sqlite3/src/better_sqlite3.o := g++ -o Release/obj.target/better_sqlite3/src/better_sqlite3.o ../src/better_sqlite3.cpp '-DNODE_GYP_MODULE_NAME=better_sqlite3' '-DUSING_UV_SHARED=1' '-DUSING_V8_SHARED=1' '-DV8_DEPRECATION_WARNINGS=1' '-D_GLIBCXX_USE_CXX11_ABI=1' '-D_FILE_OFFSET_BITS=64' '-D_LARGEFILE_SOURCE' '-D__STDC_FORMAT_MACROS' '-DOPENSSL_NO_PINSHARED' '-DOPENSSL_THREADS' '-DNAPI_VERSION=8' '-DNODE_ADDON_API_DISABLE_CPP_EXCEPTIONS' '-DBUILDING_NODE_EXTENSION' '-DNDEBUG' -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/src -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/deps/openssl/config -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/deps/openssl/openssl/include -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/deps/uv/include -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/deps/zlib -I/home/vercel-sandbox/.cache/node-gyp/24.16.0/deps/v8/include -I/vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api -I./Release/obj/gen/sqlite3  -fPIC -pthread -Wall -Wextra -Wno-unused-parameter -m64 -O3 -O3 -fno-omit-frame-pointer -fno-rtti -fno-exceptions -fno-strict-aliasing -std=gnu++20 -std=c++20 -fvisibility=hidden -fvisibility-inlines-hidden -flto -MMD -MF ./Release/.deps/Release/obj.target/better_sqlite3/src/better_sqlite3.o.d.raw   -c
Release/obj.target/better_sqlite3/src/better_sqlite3.o: \
 ../src/better_sqlite3.cpp Release/obj/gen/sqlite3/sqlite3.h \
 /vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/node_api.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/js_native_api.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/js_native_api_types.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/node_api_types.h \
 /vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi-inl.h \
 /vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi.h \
 /vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi-inl.deprecated.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/errno.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/version.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/unix.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/threadpool.h \
 /home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/linux.h \
 ../src/util/macros.cpp ../src/util/helpers.cpp ../src/util/constants.cpp \
 ../src/util/bind-map.cpp ../src/util/data-converter.cpp \
 ../src/util/row-builder.hpp ../src/objects/backup.hpp \
 ../src/objects/statement.hpp ../src/objects/write-coordinator.hpp \
 ../src/objects/database.hpp ../src/addon.cpp \
 ../src/objects/statement-iterator.hpp ../src/util/data.cpp \
 ../src/util/row-builder.cpp ../src/util/query-macros.cpp \
 ../src/util/custom-function.cpp ../src/util/custom-aggregate.cpp \
 ../src/util/custom-table.cpp ../src/util/binder.cpp \
 ../src/objects/backup.cpp ../src/objects/statement.cpp \
 ../src/objects/database.cpp ../src/objects/write-coordinator.cpp \
 ../src/objects/statement-iterator.cpp
../src/better_sqlite3.cpp:
Release/obj/gen/sqlite3/sqlite3.h:
/vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/node_api.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/js_native_api.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/js_native_api_types.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/node_api_types.h:
/vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi-inl.h:
/vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi.h:
/vercel/share/v0-project/node_modules/.pnpm/node-addon-api@8.9.2/node_modules/node-addon-api/napi-inl.deprecated.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/errno.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/version.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/unix.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/threadpool.h:
/home/vercel-sandbox/.cache/node-gyp/24.16.0/include/node/uv/linux.h:
../src/util/macros.cpp:
../src/util/helpers.cpp:
../src/util/constants.cpp:
../src/util/bind-map.cpp:
../src/util/data-converter.cpp:
../src/util/row-builder.hpp:
../src/objects/backup.hpp:
../src/objects/statement.hpp:
../src/objects/write-coordinator.hpp:
../src/objects/database.hpp:
../src/addon.cpp:
../src/objects/statement-iterator.hpp:
../src/util/data.cpp:
../src/util/row-builder.cpp:
../src/util/query-macros.cpp:
../src/util/custom-function.cpp:
../src/util/custom-aggregate.cpp:
../src/util/custom-table.cpp:
../src/util/binder.cpp:
../src/objects/backup.cpp:
../src/objects/statement.cpp:
../src/objects/database.cpp:
../src/objects/write-coordinator.cpp:
../src/objects/statement-iterator.cpp:
