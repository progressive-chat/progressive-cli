#!/usr/bin/env node
// docs/gen.mjs — stamps docs/status.json with the version/commit/build of
// the CURRENT tree and resolves every feature anchor to its line number.
// Runs with node builtins only (no npm install).
//
//   node docs/gen.mjs            -> writes docs/status.json
//   node docs/gen.mjs --json     -> prints it to stdout instead

import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const docsDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(docsDir, '..');

const sh = (cmd, args) =>
    execFileSync(cmd, args, { cwd: repoRoot, encoding: 'utf8' }).trim();

// The version marker: the nearest tag + the distance + the short hash —
// that ties the page to a version AND a commit. The build date stamps the
// generation time.
let version = 'unknown', commit = 'unknown';
try {
    version = sh('git', ['describe', '--tags', '--always', '--long', '--dirty']);
} catch {}
try {
    commit = sh('git', ['rev-parse', 'HEAD']);
} catch {}

const builtAt = new Date().toISOString();

const featuresJson = JSON.parse(readFileSync(join(docsDir, 'features.json'), 'utf8'));

function resolveAnchor(anchor) {
    if (!anchor || !anchor.file || !anchor.pattern) return null;
    const path = join(repoRoot, 'cppcli', anchor.file);
    let text;
    try {
        text = readFileSync(path, 'utf8');
    } catch {
        return { ...anchor, missing: true, line: -1 };
    }
    const rx = new RegExp(anchor.pattern, 'm');
    const m = rx.exec(text);
    if (!m) return { ...anchor, missing: true, line: -1 };
    const line = text.slice(0, m.index).split('\n').length;
    return { ...anchor, missing: false, line };
}

const features = featuresJson.features.map((f) => {
    const resolved = f.anchor ? resolveAnchor(f.anchor) : null;
    return { ...f, anchor: resolved || undefined };
});

const status = {
    generated: { version, commit, builtAt, generator: 'docs/gen.mjs' },
    note: 'This file is generated. Edit docs/features.json instead.',
    categories: featuresJson.categories,
    features,
};

const out = JSON.stringify(status, null, 2) + '\n';
if (process.argv.includes('--json')) {
    process.stdout.write(out);
} else {
    writeFileSync(join(docsDir, 'status.json'), out);
    console.log(`docs/status.json written (${version}, ${commit.slice(0, 7)})`);
}

const missing = features.filter((f) => f.anchor && f.anchor.missing);
if (missing.length) {
    console.error(`WARNING: ${missing.length} anchor(s) unresolved:`);
    for (const f of missing) console.error(`  - ${f.id} (${f.anchor.file}: ${f.anchor.pattern})`);
}
