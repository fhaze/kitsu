import assert from 'node:assert/strict';
import { readFile, readdir, stat } from 'node:fs/promises';
import { join, resolve, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../dist/', import.meta.url));
const origin = 'https://fhaze.github.io';
const base = '/kitsu/';
const required = [
    'index.html',
    'docs/index.html',
    'install/index.html',
    'windows/index.html',
    'troubleshooting/index.html',
    '404.html',
    'sitemap.xml',
    'kitsu.png',
    'kitsu-128.png',
];
for (const name of required)
    assert.ok((await stat(join(root, name))).isFile(), `Missing ${name}`);

async function walk(directory) {
    const entries = await readdir(directory, { withFileTypes: true });
    return (
        await Promise.all(
            entries.map((entry) =>
                entry.isDirectory()
                    ? walk(join(directory, entry.name))
                    : join(directory, entry.name),
            ),
        )
    ).flat();
}

const files = await walk(root);
const contents = new Map();
for (const file of files.filter((file) =>
    ['.html', '.css'].includes(extname(file)),
)) {
    contents.set(resolve(file), await readFile(file, 'utf8'));
}
let checked = 0;
const failures = [];
for (const [file, html] of contents) {
    if (extname(file) === '.css') {
        for (const match of html.matchAll(/url\(["']?([^\s)"']+)["']?\)/g)) {
            if (/^(?:data:|https?:)/.test(match[1])) continue;
            const stylesheet = new URL(
                `${base}${file.slice(root.length).replaceAll('\\', '/')}`,
                origin,
            );
            const target = new URL(match[1], stylesheet);
            assert.ok(
                target.pathname.startsWith(base),
                `CSS asset escapes ${base}`,
            );
            await stat(
                resolve(
                    root,
                    decodeURIComponent(target.pathname.slice(base.length)),
                ),
            );
            checked++;
        }
        continue;
    }
    const relative = file.slice(root.length).replaceAll('\\', '/');
    const pageUrl = new URL(
        `${base}${relative.replace(/index\.html$/, '')}`,
        origin,
    );
    const tags = html.match(/<(?:a|img|script|link)\b[^>]*>/g) ?? [];
    for (const tag of tags) {
        if (/rel="canonical"/.test(tag)) continue;
        const attribute = /(?:href|src)="([^"]*)"/.exec(tag);
        if (
            !attribute ||
            !attribute[1] ||
            /^(?:mailto:|tel:|data:)/.test(attribute[1])
        )
            continue;
        const target = new URL(attribute[1].replaceAll('&amp;', '&'), pageUrl);
        if (target.origin !== origin) continue;
        try {
            assert.ok(
                target.pathname.startsWith(base),
                `Link escapes ${base}: ${attribute[1]}`,
            );
            const pathname = decodeURIComponent(
                target.pathname.slice(base.length),
            );
            let destination = resolve(root, pathname);
            assert.ok(
                destination.startsWith(resolve(root)),
                'Link escapes output directory',
            );
            const info = await stat(destination);
            if (info.isDirectory())
                destination = join(destination, 'index.html');
            await stat(destination);
            if (target.hash) {
                const text =
                    contents.get(resolve(destination)) ??
                    (await readFile(destination, 'utf8'));
                const id = decodeURIComponent(target.hash.slice(1));
                assert.ok(text.includes(`id="${id}"`), `Missing anchor ${id}`);
            }
            checked++;
        } catch (error) {
            failures.push(`${relative}: ${attribute[1]} — ${error.message}`);
        }
    }
    assert.ok(
        html.includes('name="description"'),
        `${relative} needs a description`,
    );
    assert.ok(
        html.includes('<html lang="en">'),
        `${relative} needs a language`,
    );
    assert.equal(
        (html.match(/<h1\b/g) ?? []).length,
        1,
        `${relative} should have one h1`,
    );
}
assert.equal(failures.length, 0, `Broken local links:\n${failures.join('\n')}`);
console.log(
    `Checked ${checked} local links and assets across ${files.filter((file) => extname(file) === '.html').length} pages. All routes stay under ${base}.`,
);
