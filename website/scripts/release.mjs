export const releasesUrl = 'https://github.com/fhaze/kitsu/releases';

export const fallbackRelease = {
    tag: null,
    published: null,
    url: `${releasesUrl}/latest`,
    installer: null,
    portable: null,
};

export function selectRelease(release) {
    if (!release || release.draft || release.prerelease || !release.tag_name) {
        throw new Error('Expected a published stable Kitsu release.');
    }
    const asset = (pattern) => {
        const matches = (release.assets ?? []).filter((item) =>
            pattern.test(item.name),
        );
        if (matches.length !== 1)
            throw new Error(`Expected one release asset matching ${pattern}.`);
        const url = new URL(matches[0].browser_download_url);
        if (
            url.origin !== 'https://github.com' ||
            !url.pathname.startsWith('/fhaze/kitsu/releases/download/')
        ) {
            throw new Error('Unexpected release asset URL.');
        }
        return url.href;
    };
    return {
        tag: release.tag_name,
        published: release.published_at,
        url: `https://github.com/fhaze/kitsu/releases/tag/${encodeURIComponent(release.tag_name)}`,
        installer: asset(/^kitsu-.+-windows-x86_64-setup\.exe$/),
        portable: asset(/^kitsu-.+-windows-x86_64\.zip$/),
    };
}
