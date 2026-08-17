'use strict';
// Install-time binary resolution.
//
// npm/pnpm implicitly run `node-gyp rebuild` for any package that ships a
// binding.gyp at its root. binding.gyp already turns the targets into
// `type: none` when a prebuild exists, but node-gyp still shells out to
// `make`/`python3` to execute that no-op, so a consumer without a toolchain
// still fails to install. Registering this file as the "install" script
// replaces that implicit rebuild, so when a committed prebuild works we exit 0
// without ever invoking node-gyp.
//
// Order of preference:
//   1. A prebuild in prebuilds/ that actually loads here -> done, no compiler.
//   2. An existing build/Release (or Debug) binary that loads -> done.
//   3. Otherwise compile with node-gyp; only then can the install fail.

const { existsSync } = require('fs');
const { join } = require('path');
const { spawnSync } = require('child_process');

const root = join(__dirname, '..');
const log = (msg) => console.log(`[better-sqlite3] ${msg}`);

// Actually dlopen the candidate in a child process. A file that merely exists
// may target another libc/arch/ABI; treating it as usable would skip the
// compile and leave the consumer with a module that throws at require() time.
function loads(file) {
  if (!existsSync(file)) return false;
  return spawnSync(
    process.execPath,
    ['-e', 'process.dlopen({ exports: {} }, process.argv[1])', file],
    { cwd: root, stdio: 'ignore' }
  ).status === 0;
}

function tryPrebuilt() {
  let getPrebuildPath;
  try {
    ({ getPrebuildPath } = require(join(root, 'lib', 'binding.js')));
  } catch {
    return false;
  }
  if (typeof getPrebuildPath !== 'function') return false;

  let file = null;
  try {
    file = getPrebuildPath();
  } catch {
    return false;
  }
  if (!file) return false;

  if (loads(file)) {
    log(`using prebuilt binary ${file} - skipping compilation`);
    return true;
  }
  log(`prebuilt binary ${file} is not loadable here; building from source`);
  return false;
}

function tryExistingBuild() {
  for (const cfg of ['Release', 'Debug']) {
    if (loads(join(root, 'build', cfg, 'better_sqlite3.node'))) {
      log(`reusing existing build/${cfg} binary`);
      return true;
    }
  }
  return false;
}

function build() {
  log('no usable prebuilt binary; compiling from source with node-gyp...');
  const attempts = [];
  try {
    attempts.push([process.execPath, [require.resolve('node-gyp/bin/node-gyp.js'), 'rebuild', '--release', '--force_build=1']]);
  } catch {
    // node-gyp is not resolvable from this package; fall through to PATH.
  }
  attempts.push(['node-gyp', ['rebuild', '--release', '--force_build=1']]);

  for (const [cmd, args] of attempts) {
    const res = spawnSync(cmd, args, { cwd: root, stdio: 'inherit', shell: process.platform === 'win32' });
    if (res.status === 0) return true;
    // Only advance to the next candidate when this one was not found at all.
    if (res.error && res.error.code === 'ENOENT') continue;
    return false;
  }

  console.error('[better-sqlite3] node-gyp was not found and no prebuilt binary matches this platform.');
  return false;
}

if (tryPrebuilt() || tryExistingBuild()) process.exit(0);
process.exit(build() ? 0 : 1);
