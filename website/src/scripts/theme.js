// Run in the document head so a saved dark theme is applied before first paint.
(() => {
    const root = document.documentElement;
    const system = window.matchMedia('(prefers-color-scheme: dark)');
    const storageKey = 'kitsu-theme';
    let preference = null;

    try {
        const saved = localStorage.getItem(storageKey);
        if (saved === 'light' || saved === 'dark') preference = saved;
    } catch {
        // The theme still works when browser storage is unavailable.
    }

    function applyTheme(theme) {
        root.dataset.theme = theme;
        root.style.colorScheme = theme;
        document
            .querySelector('meta[name="theme-color"]')
            ?.setAttribute('content', theme === 'dark' ? '#191c18' : '#f8f7f3');
        const button = document.querySelector('[data-theme-toggle]');
        if (button) {
            const label = `Switch to ${theme === 'dark' ? 'light' : 'dark'} theme`;
            button.setAttribute('aria-label', label);
            button.setAttribute('title', label);
            button.setAttribute('aria-pressed', String(theme === 'dark'));
        }
    }

    applyTheme(preference ?? (system.matches ? 'dark' : 'light'));

    system.addEventListener('change', (event) => {
        if (preference === null) applyTheme(event.matches ? 'dark' : 'light');
    });

    document.addEventListener('DOMContentLoaded', () => {
        const button = document.querySelector('[data-theme-toggle]');
        if (!button) return;
        button.hidden = false;
        applyTheme(root.dataset.theme);
        button.addEventListener('click', () => {
            preference = root.dataset.theme === 'dark' ? 'light' : 'dark';
            applyTheme(preference);
            try {
                localStorage.setItem(storageKey, preference);
            } catch {
                // Keep the choice for this page even if it cannot be persisted.
            }
        });
    });
})();
