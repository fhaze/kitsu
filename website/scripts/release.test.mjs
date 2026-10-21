import test from 'node:test';
import assert from 'node:assert/strict';
import { fallbackRelease, selectRelease } from './release.mjs';

const fixture = () => ({
    tag_name: 'v0.49.2-windows.15',
    draft: false,
    prerelease: false,
    published_at: '2026-10-21T19:20:35Z',
    assets: [
        {
            name: 'kitsu-0.49.2-windows-x86_64-setup.exe',
            browser_download_url:
                'https://github.com/fhaze/kitsu/releases/download/v0.49.2-windows.15/kitsu-0.49.2-windows-x86_64-setup.exe',
        },
        {
            name: 'kitsu-0.49.2-windows-x86_64-setup.exe.sig',
            browser_download_url:
                'https://github.com/fhaze/kitsu/releases/download/v0.49.2-windows.15/kitsu-0.49.2-windows-x86_64-setup.exe.sig',
        },
        {
            name: 'kitsu-0.49.2-windows-x86_64.zip',
            browser_download_url:
                'https://github.com/fhaze/kitsu/releases/download/v0.49.2-windows.15/kitsu-0.49.2-windows-x86_64.zip',
        },
    ],
});

test('uses real asset URLs when the tag and package versions differ', () => {
    const release = selectRelease(fixture());
    assert.equal(release.tag, 'v0.49.2-windows.15');
    assert.equal(release.installer, fixture().assets[0].browser_download_url);
    assert.equal(release.portable, fixture().assets[2].browser_download_url);
});

test('does not promote drafts or prereleases as stable downloads', () => {
    assert.throws(() => selectRelease({ ...fixture(), draft: true }));
    assert.throws(() => selectRelease({ ...fixture(), prerelease: true }));
});

test('rejects missing or ambiguous downloads instead of creating broken buttons', () => {
    const release = fixture();
    release.assets.pop();
    assert.throws(() => selectRelease(release), /Expected one release asset/);
    const duplicate = fixture();
    duplicate.assets.push(duplicate.assets[0]);
    assert.throws(() => selectRelease(duplicate), /Expected one release asset/);
});

test('rejects download links outside the Kitsu repository', () => {
    const release = fixture();
    release.assets[0].browser_download_url =
        'https://example.com/installer.exe';
    assert.throws(() => selectRelease(release), /Unexpected release asset URL/);
});

test('offline builds use a working release page instead of invented download URLs', () => {
    assert.equal(
        fallbackRelease.url,
        'https://github.com/fhaze/kitsu/releases/latest',
    );
    assert.equal(fallbackRelease.tag, null);
    assert.equal(fallbackRelease.installer, null);
    assert.equal(fallbackRelease.portable, null);
});
