(function () {
  const STORAGE_KEY = 'pm-theme';
  const ACCENT_KEY = 'pm-accent';

  function apply(theme, accent) {
    document.documentElement.setAttribute('data-theme', theme);
    if (accent) document.documentElement.setAttribute('data-accent', accent);
  }

  function getSaved() {
    let theme = localStorage.getItem(STORAGE_KEY);
    let accent = localStorage.getItem(ACCENT_KEY);
    if (!theme) {
      const urlParams = new URLSearchParams(window.location.search);
      theme = urlParams.get('theme') || (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    }
    return { theme, accent };
  }

  function toggle() {
    const current = document.documentElement.getAttribute('data-theme') || 'dark';
    const next = current === 'dark' ? 'light' : 'dark';
    apply(next);
    localStorage.setItem(STORAGE_KEY, next);
    updateToggleLabel();
  }

  function setAccent(accent) {
    apply(document.documentElement.getAttribute('data-theme') || 'dark', accent);
    localStorage.setItem(ACCENT_KEY, accent);
    document.querySelectorAll('.accent-dot').forEach(el => el.classList.remove('active'));
    const active = document.querySelector(`.accent-dot[data-accent="${accent}"]`);
    if (active) active.classList.add('active');
  }

  function updateToggleLabel() {
    const current = document.documentElement.getAttribute('data-theme') || 'dark';
    const label = document.getElementById('theme-label');
    if (label) label.textContent = current === 'dark' ? 'Dark' : 'Light';
  }

  function applyUrlParams() {
    const params = new URLSearchParams(window.location.search);
    const theme = params.get('theme');
    const accent = params.get('accent');
    if (theme) {
      apply(theme);
      localStorage.setItem(STORAGE_KEY, theme);
    }
    if (accent) setAccent(accent);
  }

  // Init before paint to avoid flash
  const { theme, accent } = getSaved();
  apply(theme, accent);

  document.addEventListener('DOMContentLoaded', function () {
    updateToggleLabel();
    applyUrlParams();

    document.querySelectorAll('.theme-toggle').forEach(btn => {
      btn.addEventListener('click', toggle);
    });

    document.querySelectorAll('.accent-dot').forEach(dot => {
      dot.addEventListener('click', function () {
        setAccent(this.getAttribute('data-accent'));
      });
    });
  });
})();
