#!/usr/bin/env node
// docs/check.mjs — the freshness checker. Verifies that a generated page
// (status.json) faithfully describes the code it claims to describe, and
// reports how far the CURRENT tree has drifted from it.
//
//   node docs/check.mjs                        # local: features.json vs the tree
//   node docs/check.mjs --gen                  # regenerate status.json first
//   node docs/check.mjs --remote <status.json URL>
//      # fetch a PUBLISHED page and verify it against the commit it was
//      # generated from (via 'git show <commit>:<file>'), plus a drift
//      # report against the working tree.
//
// Exit codes: 0 = faithful; 1 = the doc is broken (an anchor no longer
// exists at the recorded commit); 2 = local drift (the working tree moved
// on — the page needs a regen); 3 = usage error.

import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const docsDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(docsDir, '..');

const args = process.argv.slice(2);
const gen = args.includes('--gen');
const remoteIdx = args.indexOf('--remote');
const remote = remoteIdx !== -1 ? args[remoteIdx + 1] : null;
if (remoteIdx !== -1 && !remote) {
    console.error('usage: check.mjs [--gen] [--remote <url>]');
    process.exit(3);
}

function readTreeFile(rel) {
    return readFileSync(join(repoRoot, 'cppcli', rel), 'utf8');
}

function readAtCommit(commit, rel) {
    try {
        return execFileSync('git', ['show', `${commit}:cppcli/${rel}`],
                            { cwd: repoRoot, encoding: 'utf8' });
    } catch {
        return null;
    }
}

function anchorHolds(text, pattern) {
    return new RegExp(pattern, 'm').test(text);
}

function anchorLine(text, pattern) {
    const m = new RegExp(pattern, 'm').exec(text);
    return m ? text.slice(0, m.index).split('\n').length : -1;
}

let status;
if (remote) {
    const res = await fetch(remote);
    if (!res.ok) {
        console.error(`cannot fetch ${remote}: HTTP ${res.status}`);
        process.exit(3);
    }
    status = await res.json();
} else {
    if (gen) {
        execFileSync(process.execPath, [join(docsDir, 'gen.mjs')], { stdio: 'inherit' });
    }
    status = JSON.parse(readFileSync(join(docsDir, 'status.json'), 'utf8'));
}

const { version, commit } = status.generated;
console.log(`page:  version=${version} commit=${commit ? commit.slice(0, 7) : 'n/a'}`);

let headCommit = null;
try {
    headCommit = execFileSync('git', ['rev-parse', 'HEAD'],
                              { cwd: repoRoot, encoding: 'utf8' }).trim();
} catch {}

let broken = 0, drift = 0, checked = 0;

for (const f of status.features) {
    const a = f.anchor;
    if (!a || !a.file || !a.pattern) continue;
    checked++;

    // 1. Faithfulness: does the anchor hold in the code the page describes?
    let textAtCommit = null;
    if (remote && commit && commit !== 'unknown') {
        textAtCommit = readAtCommit(commit, a.file);
    } else {
        textAtCommit = readTreeFile(a.file);  // local status.json: the tree IS the source
    }
    if (textAtCommit === null || !anchorHolds(textAtCommit, a.pattern)) {
        console.error(`BROKEN ${f.id}: "${a.pattern}" missing in ${a.file}@${remote ? commit.slice(0, 7) : 'HEAD'}`);
        broken++;
        continue;
    }

    // 2. Drift: where does the anchor sit in the working tree now?
    let treeText = null;
    try {
        treeText = readTreeFile(a.file);
    } catch {}
    if (treeText === null || !anchorHolds(treeText, a.pattern)) {
        console.warn(`drift ${f.id}: "${a.pattern}" gone from the working tree (${a.file})`);
        drift++;
    } else if (remote) {
        const lineNow = anchorLine(treeText, a.pattern);
        const lineThen = a.line ?? -1;
        if (lineNow !== -1 && lineThen !== -1 && lineNow !== lineThen) {
            console.warn(`drift ${f.id}: ${a.file}:${lineThen} -> :${lineNow} (content still matches)`);
            drift++;
        }
    }
}

if (remote && headCommit && commit && commit !== 'unknown' && headCommit !== commit) {
    console.warn(`drift: the page was generated at ${commit.slice(0, 7)}, HEAD is ${headCommit.slice(0, 7)}`);
    drift++;
}

console.log(`${checked} anchor(s) checked, ${broken} broken, ${drift} drifted`);
if (broken) {
    console.error('FAIL: the page does not match the code it describes.');
    process.exit(1);
}
if (drift) {
    console.error('WARN: the page lags the tree — regenerate it (node docs/gen.mjs).');
    process.exit(2);
}
console.log('OK: the page matches its code.');
