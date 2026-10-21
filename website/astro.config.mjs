import { defineConfig } from 'astro/config';

export default defineConfig({
    site: 'https://fhaze.github.io',
    base: '/kitsu',
    output: 'static',
    trailingSlash: 'always',
    devToolbar: { enabled: false },
    markdown: { shikiConfig: { theme: 'github-dark' } },
});
