import type { APIRoute } from 'astro';

export const GET: APIRoute = ({ site }) => {
    const root = `${site!.origin}${import.meta.env.BASE_URL.replace(/\/$/, '')}/`;
    const paths = ['', 'docs/', 'install/', 'windows/', 'troubleshooting/'];
    return new Response(
        `<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">${paths.map((path) => `<url><loc>${root}${path}</loc></url>`).join('')}</urlset>`,
        { headers: { 'Content-Type': 'application/xml' } },
    );
};
