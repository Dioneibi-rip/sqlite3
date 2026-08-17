'use strict';
const Database = require('../lib');

describe('async concurrency', function () {
	beforeEach(function () {
		this.db = new Database(util.next());
		this.db.pragma('journal_mode = WAL');
		this.db.pragma('synchronous = NORMAL');
		this.db.exec('CREATE TABLE entries (a INTEGER PRIMARY KEY, b TEXT)');
	});
	afterEach(function () {
		this.db.close();
	});

	it('should allow concurrent runAsync() on the same statement', async function () {
		const stmt = this.db.prepare('INSERT INTO entries (b) VALUES (?)');
		const results = await Promise.all(
			Array.from({ length: 50 }, (_, i) => stmt.runAsync(`row-${i}`))
		);
		expect(results.length).to.equal(50);
		for (const result of results) expect(result.changes).to.equal(1);
		expect(this.db.prepare('SELECT COUNT(*) AS c FROM entries').pluck().get()).to.equal(50);
	});

	it('should allow concurrent allAsync()/getAsync() on the same statement', async function () {
		const insert = this.db.prepare('INSERT INTO entries (b) VALUES (?)');
		for (let i = 0; i < 20; ++i) insert.run(`row-${i}`);

		const all = this.db.prepare('SELECT * FROM entries WHERE a > ?');
		const get = this.db.prepare('SELECT b FROM entries WHERE a = ?');
		const [rows, singles] = await Promise.all([
			Promise.all(Array.from({ length: 25 }, () => all.allAsync(0))),
			Promise.all(Array.from({ length: 25 }, (_, i) => get.getAsync((i % 20) + 1))),
		]);
		for (const set of rows) expect(set.length).to.equal(20);
		for (let i = 0; i < singles.length; ++i) expect(singles[i].b).to.equal(`row-${i % 20}`);
	});

	it('should interleave concurrent reads and writes without locking errors', async function () {
		const insert = this.db.prepare('INSERT INTO entries (b) VALUES (?)');
		const read = this.db.prepare('SELECT COUNT(*) AS c FROM entries');
		const jobs = [];
		for (let i = 0; i < 40; ++i) {
			jobs.push(insert.runAsync(`row-${i}`));
			jobs.push(read.getAsync());
		}
		await Promise.all(jobs);
		expect(this.db.prepare('SELECT COUNT(*) AS c FROM entries').pluck().get()).to.equal(40);
	});

	it('should stay usable synchronously after concurrent async jobs settle', async function () {
		const stmt = this.db.prepare('INSERT INTO entries (b) VALUES (?)');
		await Promise.all(Array.from({ length: 10 }, (_, i) => stmt.runAsync(`row-${i}`)));
		expect(stmt.run('sync').changes).to.equal(1);
		expect(this.db.prepare('SELECT COUNT(*) AS c FROM entries').pluck().get()).to.equal(11);
	});

	it('should reject each concurrent job that fails without poisoning the others', async function () {
		this.db.exec('CREATE TABLE uniq (a INTEGER PRIMARY KEY)');
		const stmt = this.db.prepare('INSERT INTO uniq (a) VALUES (?)');
		const settled = await Promise.allSettled([
			stmt.runAsync(1),
			stmt.runAsync(1),
			stmt.runAsync(2),
		]);
		const rejected = settled.filter((x) => x.status === 'rejected');
		expect(rejected.length).to.equal(1);
		expect(rejected[0].reason.code).to.equal('SQLITE_CONSTRAINT_PRIMARYKEY');
		expect(this.db.prepare('SELECT COUNT(*) AS c FROM uniq').pluck().get()).to.equal(2);
	});

	it('should not add fixed timer latency to a single async write', async function () {
		const stmt = this.db.prepare('INSERT INTO entries (b) VALUES (?)');
		await stmt.runAsync('warmup');
		const start = process.hrtime.bigint();
		for (let i = 0; i < 20; ++i) await stmt.runAsync(`row-${i}`);
		const perOpMs = Number(process.hrtime.bigint() - start) / 1e6 / 20;
		expect(perOpMs).to.be.below(4);
	});
});
