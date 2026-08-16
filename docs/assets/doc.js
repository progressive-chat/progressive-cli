// docs/assets/doc.js — renders status.json into the header meta and the
// feature table. Plain DOM, no dependencies.

const STATUS_LABELS = {
    verified: 'Proved working',
    implemented: 'Implemented',
    awaited: 'Awaited',
    wontadd: "Won't add",
};

const STATUS_ORDER = ['verified', 'implemented', 'awaited', 'wontadd'];

const GH = 'https://github.com/progressive-chat/progressive-cli';

async function boot() {
    const res = await fetch('status.json');
    const status = await res.json();
    const { version, commit, builtAt } = status.generated;

    document.querySelectorAll('[data-version]').forEach((el) => {
        el.textContent = version;
        const el2 = el.querySelector('[data-version]');
        if (el2) el2.textContent = version;
    });
    document.querySelectorAll('[data-commit]').forEach((el) => {
        el.textContent = commit ? commit.slice(0, 7) : 'n/a';
        el.href = commit && commit !== 'unknown' ? `${GH}/commit/${commit}` : GH;
    });
    document.querySelectorAll('[data-built]').forEach((el) => {
        el.textContent = builtAt ? new Date(builtAt).toUTCString() : 'n/a';
    });

    const tbody = document.getElementById('rows');
    if (!tbody) return;
    const rows = status.features.map((f) => ({ f, tr: document.createElement('tr') }));
    rows.forEach(({ f, tr }) => {
        const cat = document.createElement('td');
        cat.textContent = f.category;
        const name = document.createElement('td');
        name.textContent = f.name;
        const st = document.createElement('td');
        const badge = document.createElement('span');
        badge.className = `badge ${f.status}`;
        badge.textContent = STATUS_LABELS[f.status] || f.status;
        st.appendChild(badge);
        const since = document.createElement('td');
        since.textContent = f.since || '—';
        const info = document.createElement('td');
        if (f.test) {
            const t = document.createElement('div');
            t.className = 'note';
            t.textContent = `test: ${f.test}`;
            info.appendChild(t);
        }
        if (f.note) {
            const n = document.createElement('div');
            n.className = 'note';
            n.textContent = f.note;
            info.appendChild(n);
        }
        if (f.anchor && !f.anchor.missing) {
            const a = document.createElement('div');
            a.className = 'anchor';
            const ref = commit && commit !== 'unknown' ? commit : 'main';
            const link = document.createElement('a');
            link.href = `${GH}/blob/${ref}/cppcli/${f.anchor.file}#L${f.anchor.line}`;
            link.textContent = `${f.anchor.file}:${f.anchor.line}`;
            a.appendChild(link);
            info.appendChild(a);
        }
        tr.dataset.status = f.status;
        tr.append(cat, name, st, since, info);
        tbody.appendChild(tr);
    });

    document.querySelectorAll('.filters button').forEach((btn) => {
        btn.addEventListener('click', () => {
            const on = btn.classList.toggle('on');
            document.querySelectorAll('.filters button').forEach((b) => {
                if (b !== btn) b.classList.remove('on');
            });
            const sel = on ? btn.dataset.status : null;
            document.querySelectorAll('#rows tr').forEach((tr) => {
                tr.classList.toggle('hidden', !!sel && tr.dataset.status !== sel);
            });
        });
    });

    const counts = {};
    for (const f of status.features) counts[f.status] = (counts[f.status] || 0) + 1;
    STATUS_ORDER.forEach((s) => {
        document.querySelectorAll(`[data-count-${s}]`).forEach((el) => {
            el.textContent = counts[s] || 0;
        });
    });
}

document.addEventListener('DOMContentLoaded', boot);
