import metadata from '../../.generated/release.json';

export const release = metadata as {
    tag: string | null;
    published: string | null;
    url: string;
    installer: string | null;
    portable: string | null;
};
export const repository = 'https://github.com/fhaze/kitsu';
export const upstream = 'https://sw.kovidgoyal.net/kitty/';
export const url = (path = '') =>
    `${import.meta.env.BASE_URL.replace(/\/$/, '')}/${path.replace(/^\//, '')}`;
export const installerUrl = release.installer ?? release.url;
export const portableUrl = release.portable ?? release.url;
