import { copyFile, mkdir, writeFile } from 'node:fs/promises';
import { fallbackRelease, selectRelease } from './release.mjs';

const root = new URL('../', import.meta.url);
await mkdir(new URL('public/', root), { recursive: true });
await mkdir(new URL('.generated/', root), { recursive: true });
for (const name of ['kitsu.png', 'kitsu-128.png']) {
    await copyFile(
        new URL(`../logo/${name}`, root),
        new URL(`public/${name}`, root),
    );
}

let release = fallbackRelease;
try {
    const headers = {
        Accept: 'application/vnd.github+json',
        'User-Agent': 'kitsu-website',
    };
    if (process.env.GH_TOKEN)
        headers.Authorization = `Bearer ${process.env.GH_TOKEN}`;
    const response = await fetch(
        'https://api.github.com/repos/fhaze/kitsu/releases/latest',
        {
            headers,
            signal: AbortSignal.timeout(12_000),
        },
    );
    if (!response.ok)
        throw new Error(`GitHub returned HTTP ${response.status}.`);
    release = selectRelease(await response.json());
    console.log(`Using release ${release.tag}.`);
} catch (error) {
    if (process.env.KITSU_REQUIRE_RELEASE === '1') throw error;
    console.warn(
        `Release metadata unavailable; linking to GitHub Releases. ${error.message}`,
    );
}
await writeFile(
    new URL('.generated/release.json', root),
    `${JSON.stringify(release, null, 2)}\n`,
);
