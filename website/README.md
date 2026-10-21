# Kitsu website

The static Kitsu website is built with Astro and deployed to
<https://fhaze.github.io/kitsu/> by `.github/workflows/pages.yml`.
It does not build or import the terminal runtime.

## Local development

Use Node.js 22.23.2 or a compatible newer version, and npm:

```sh
cd website
npm ci
npm run dev
```

Open the printed local URL, including `/kitsu/`. The dev server binds to loopback.

```sh
npm test
npm run build
npm run check
npm run preview
```

`prepare-site` copies the existing fox logo from `../logo/` and retrieves the
latest stable GitHub release. It chooses the installer and ZIP by their real
asset URLs because release tags and package filenames have different versions.
The generated metadata, logo copies, dependencies, and build output are ignored.

Without network access, local builds link to the GitHub latest-release page
instead of inventing version numbers or download URLs. Set `KITSU_REQUIRE_RELEASE=1`
to fail if metadata is unavailable; CI uses this mode. `GH_TOKEN` is optional for
local rate-limit authentication and is never written into the built website.

## Content

- `src/pages/index.astro`: product homepage and download calls to action.
- `src/pages/docs.astro`: guide index and upstream Kitty references.
- `src/pages/*.md`: Windows guides using the shared `Guide.astro` layout.
- `src/styles/global.css`: responsive design, including documentation.
- `src/components/TerminalDemo.astro`: accessible session tabs; the demo is an
  explicitly labeled illustration, not a screenshot of a running terminal.

Keep supported platforms, install commands, config paths, and updater behavior
consistent with `../README.asciidoc`. The detailed diagnostics reference remains
`../docs/windows-health-check.rst`. Shared terminal reference material links to
upstream Kitty rather than duplicating the upstream Sphinx site.

All local links and assets must respect the `/kitsu/` base path. `npm run check`
checks generated pages, local links, anchors, and assets, and catches accidental
links to the account root.

## GitHub Pages setup

After merging the website into `master`, select **GitHub Actions** in repository
**Settings → Pages → Build and deployment → Source**. The `github-pages`
environment must allow deployments from `master`.

The Website workflow builds website pull requests without deploying them.
It publishes relevant `master` changes and manual runs, and refreshes release
downloads after a successful `Release` workflow using `workflow_run`. That event
checks out `master`, not the release run's source or artifacts. Published binaries
stay on GitHub Releases.

To use a custom domain later, update the Pages domain and DNS, then change `site`
and `base` in `astro.config.mjs`, `robots.txt`, and the base-path expectation in
the generated-site checker.
