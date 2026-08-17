'use strict';
// Minimal end-to-end verification of the packaged addon.
// Usage: node scripts/smoke-test.js [require-specifier]
// Defaults to requiring the package root, exercising lib/binding.js resolution.

const assert = require('assert');

const specifier = process.argv[2] || '..';
const Database = require(specifier);

const results = [];
function check(name, fn) {
	fn();
	results.push(name);
}

// Report which binary actually got loaded.
try {
	const pkgDir = require('path').dirname(require.resolve(specifier + '/package.json'));
	const { getPrebuildPath } = require(require('path').join(pkgDir, 'lib', 'binding.js'));
	console.log('prebuild resolved:', getPrebuildPath() || '(none - using build/Release)');
} catch {
	console.log('prebuild resolved: (could not introspect)');
}
console.log('node:', process.version, 'napi abi:', process.versions.modules);

check('new Database(":memory:")', () => {
	const db = new Database(':memory:');
	assert.strictEqual(db.open, true);
	assert.strictEqual(db.memory, true);
	db.close();
});

check('prepare().run() with named parameters', () => {
	const db = new Database(':memory:');
	db.exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)');

	const insert = db.prepare('INSERT INTO users (name, age) VALUES (@name, @age)');
	const info = insert.run({ name: 'Dioneibi', age: 30 });
	assert.strictEqual(info.changes, 1);
	assert.strictEqual(info.lastInsertRowid, 1);

	// $ and : prefixes must work too.
	db.prepare('INSERT INTO users (name, age) VALUES ($name, $age)').run({ name: 'Ada', age: 36 });
	db.prepare('INSERT INTO users (name, age) VALUES (:name, :age)').run({ name: 'Linus', age: 54 });

	const row = db.prepare('SELECT * FROM users WHERE name = @name').get({ name: 'Ada' });
	assert.deepStrictEqual(row, { id: 2, name: 'Ada', age: 36 });

	const all = db.prepare('SELECT name FROM users ORDER BY id').all().map((r) => r.name);
	assert.deepStrictEqual(all, ['Dioneibi', 'Ada', 'Linus']);
	db.close();
});

check("pragma('journal_mode = WAL')", () => {
	const os = require('os');
	const path = require('path');
	const fs = require('fs');
	const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'bs3-'));
	const file = path.join(dir, 'wal.db');

	const db = new Database(file);
	const mode = db.pragma('journal_mode = WAL', { simple: true });
	assert.strictEqual(mode, 'wal');
	db.exec('CREATE TABLE t (x)');
	db.prepare('INSERT INTO t (x) VALUES (?)').run(1);
	assert.ok(fs.existsSync(file + '-wal'), 'expected -wal sidecar file');
	db.close();
	fs.rmSync(dir, { recursive: true, force: true });
});

check('transaction() commit and rollback', () => {
	const db = new Database(':memory:');
	db.exec('CREATE TABLE t (x INTEGER UNIQUE)');
	const insert = db.prepare('INSERT INTO t (x) VALUES (?)');

	const many = db.transaction((values) => {
		for (const v of values) insert.run(v);
		return values.length;
	});

	assert.strictEqual(many([1, 2, 3]), 3);
	assert.strictEqual(db.prepare('SELECT COUNT(*) AS n FROM t').get().n, 3);

	// A throwing transaction must roll back entirely.
	assert.throws(() => many([4, 5, 1]));
	assert.strictEqual(db.prepare('SELECT COUNT(*) AS n FROM t').get().n, 3);

	// Nested / savepoint behaviour.
	const outer = db.transaction(() => {
		insert.run(10);
		many([11, 12]);
	});
	outer();
	assert.strictEqual(db.prepare('SELECT COUNT(*) AS n FROM t').get().n, 6);
	db.close();
});

check('user-defined function and aggregate', () => {
	const db = new Database(':memory:');
	db.function('double_it', (n) => n * 2);
	assert.strictEqual(db.prepare('SELECT double_it(21) AS v').get().v, 42);
	db.aggregate('sum_all', { start: 0, step: (total, n) => total + n });
	db.exec('CREATE TABLE n (x)');
	db.prepare('INSERT INTO n (x) VALUES (1),(2),(3)').run();
	assert.strictEqual(db.prepare('SELECT sum_all(x) AS v FROM n').get().v, 6);
	db.close();
});

check('compiled SQLite features (json1, fts5, math)', () => {
	const db = new Database(':memory:');
	assert.strictEqual(db.prepare(`SELECT json_extract('{"a":5}','$.a') AS v`).get().v, 5);
	db.exec(`CREATE VIRTUAL TABLE docs USING fts5(body)`);
	db.prepare('INSERT INTO docs (body) VALUES (?)').run('hello world');
	assert.strictEqual(db.prepare(`SELECT COUNT(*) AS n FROM docs WHERE docs MATCH 'hello'`).get().n, 1);
	assert.strictEqual(db.prepare('SELECT ceil(1.2) AS v').get().v, 2);
	console.log('sqlite version:', db.prepare('SELECT sqlite_version() AS v').get().v);
	db.close();
});

console.log('\npassed %d/%d', results.length, results.length);
for (const r of results) console.log('  ok -', r);
