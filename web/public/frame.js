// Loaded into every dictionary article frame (https://dict.dr-dict.local/<id>/@article).
// It reports the document height to the app, and turns MDict/StarDict link schemes into app
// actions: entry:// and bword:// look words up, sound:// plays audio, web links open outside.
(() => {
  const parentOrigin = 'https://app.dr-dict.local';
  const embedded = window.parent !== window;
  const post = (message) => {
    if (embedded) window.parent.postMessage({ source: 'dr-frame', ...message }, parentOrigin);
  };
  document.documentElement.classList.toggle('dr-embedded', embedded);

  let lastHeight = -1;
  let scheduled = false;
  const measure = () => {
    scheduled = false;
    const root = document.documentElement;
    const body = document.body;
    const height = Math.max(root.scrollHeight, body ? body.scrollHeight : 0, root.getBoundingClientRect().height);
    if (Math.abs(height - lastHeight) >= 1) {
      lastHeight = height;
      post({ type: 'height', height });
    }
  };
  const schedule = () => {
    if (!scheduled) {
      scheduled = true;
      requestAnimationFrame(measure);
    }
  };
  const observer = new ResizeObserver(schedule);
  document.addEventListener('DOMContentLoaded', () => {
    observer.observe(document.documentElement);
    if (document.body) observer.observe(document.body);
    new MutationObserver(schedule).observe(document.body, { subtree: true, childList: true, attributes: true });
    schedule();
  });
  window.addEventListener('load', schedule);
  // Images and fonts that finish late change the height without a DOM mutation.
  document.addEventListener('load', schedule, true);

  // An article converted to simplified/traditional ("script" in the URL) may use characters the
  // dictionary's bundled font lacks (a traditional-only Song face, say); the browser would draw
  // those in the system sans-serif and the text would look patchy. Each @font-face family the
  // dictionary declares gets one more face with the same name, declared before the dictionary's
  // own so it is consulted last (CSS Fonts: later faces of a family are checked first), and only
  // for glyphs the dictionary's fonts do not have: a Song face of the converted script.
  const script = new URLSearchParams(location.search).get('script');
  const fallbackFonts = {
    s: ['Source Han Serif SC', 'Noto Serif CJK SC', 'Noto Serif SC', 'SimSun', '宋体'],
    t: ['Source Han Serif TC', 'Noto Serif CJK TC', 'Noto Serif TC', 'PMingLiU', '新細明體', 'MingLiU'],
  }[script];
  const addFallbackFonts = () => {
    if (!fallbackFonts || !document.head) return;
    const families = new Set();
    const visit = (rules) => {
      for (const rule of rules) {
        if (rule instanceof CSSFontFaceRule) families.add(rule.style.getPropertyValue('font-family').trim());
        else if (rule.cssRules) visit(rule.cssRules);
      }
    };
    for (const sheet of document.styleSheets) {
      try {
        visit(sheet.cssRules);
      } catch {
        // A stylesheet that cannot be read declares nothing we can extend.
      }
    }
    let style = document.getElementById('dr-script-fonts');
    const known = style ? style.dataset.families.split('\n') : [];
    const missing = [...families].filter((family) => family && !known.includes(family));
    if (!missing.length) return;
    if (!style) {
      style = document.createElement('style');
      style.id = 'dr-script-fonts';
      style.dataset.families = '';
      document.head.prepend(style);
    }
    const src = fallbackFonts.map((name) => `local("${name}")`).join(', ');
    style.textContent += missing
      .map((family) => `@font-face { font-family: ${family}; src: ${src}; unicode-range: U+2E80-9FFF, U+F900-FAFF, U+FE30-FE4F, U+20000-3134F; }\n`)
      .join('');
    style.dataset.families = [...known, ...missing].join('\n');
  };
  if (fallbackFonts) {
    document.addEventListener('DOMContentLoaded', addFallbackFonts);
    window.addEventListener('load', addFallbackFonts);
  }

  let audio = null;
  // Pronunciation volume (0–1): the app passes it in the frame URL ("vol") and sends changes.
  const clampVolume = (value) => (Number.isFinite(value) ? Math.min(1, Math.max(0, value)) : 0.22);
  let volume = clampVolume(parseFloat(new URLSearchParams(location.search).get('vol') ?? ''));
  window.addEventListener('message', (event) => {
    if (event.origin !== parentOrigin || !event.data || event.data.source !== 'dr-app') return;
    if (event.data.type === 'volume') {
      volume = clampVolume(event.data.volume);
      if (audio) audio.volume = volume;
    } else if (event.data.type === 'copy') {
      // The menu's Copy, when the app could not write the clipboard itself.
      document.execCommand('copy');
    }
  });
  // A sound:// target is a resource *name*, and names may contain '#', '?' or spaces: OALDPE's
  // example audio is "sound://_take#_gbs_1.mp3". Taken as a URL, '#' would start a fragment and
  // the request would ask for "_take". GoldenDict-ng's ArticleView::playAudio likewise rejoins
  // path + "#" + fragment; here every segment is percent-encoded (after undoing any encoding the
  // dictionary already applied) and the native side decodes it back to the stored name.
  const resourceUrl = (path) =>
    new URL(
      path
        .replace(/\\/g, '/')
        .replace(/^\/+/, '')
        .split('/')
        .map((segment) => encodeURIComponent(decode(segment)))
        .join('/'),
      location.href,
    ).href;
  const play = (path) => {
    if (audio) audio.pause();
    post({ type: 'speak-stop' });
    audio = new Audio(resourceUrl(path));
    audio.volume = volume;
    audio.play().catch(() => {});
  };
  const scrollToAnchor = (name) => {
    const id = decodeURIComponent(name);
    const target = document.getElementById(id) || document.getElementsByName(id)[0];
    if (!target) return;
    const top = target.getBoundingClientRect().top + window.scrollY;
    if (embedded) post({ type: 'scroll', top });
    else target.scrollIntoView();
  };
  const decode = (text) => {
    try {
      return decodeURIComponent(text);
    } catch {
      return text;
    }
  };

  // A pronunciation icon without a sound:// target is a dictionary's own text-to-speech button.
  // OALDPE marks examples that have no recording this way ("sound pron-uk tts") and sends them
  // to Edge's online read-aloud service, which now rejects third-party clients. The app reads
  // them instead: the frame extracts the sentence, the app picks the voice.
  const speechButton = 'a.tts:not([href]), a.sound:not([href])';
  const sentenceOf = (button) => {
    const host = button.parentElement;
    if (!host) return '';
    const copy = host.cloneNode(true);
    // Drop translations (OALDPE: <xt><chn>…</chn></xt>) and the icons themselves.
    for (const node of copy.querySelectorAll('xt, chn, a.sound, a.tts, script, style')) node.remove();
    return copy.textContent.replace(/\s+/g, ' ').trim();
  };
  const accentOf = (button) => {
    const names = `${button.className} ${button.parentElement ? button.parentElement.className : ''}`;
    return /\b(pron-)?(uk|gb|br)\b/i.test(names) ? 'en-GB' : 'en-US';
  };

  document.addEventListener(
    'click',
    (event) => {
      if (event.button !== 0 || event.defaultPrevented) return;
      const speech = event.target instanceof Element ? event.target.closest(speechButton) : null;
      if (speech) {
        const text = sentenceOf(speech);
        if (!text) return;
        event.preventDefault();
        event.stopPropagation();
        if (audio) audio.pause();
        post({ type: 'speak', text, lang: accentOf(speech) });
        return;
      }
      const link = event.target instanceof Element ? event.target.closest('a[href], area[href]') : null;
      if (!link) return;
      const href = (link.getAttribute('href') || '').trim();
      const lower = href.toLowerCase();
      const handled = () => {
        event.preventDefault();
        event.stopPropagation();
      };
      if (lower.startsWith('sound://')) {
        handled();
        play(href.slice(8));
      } else if (lower.startsWith('entry://') || lower.startsWith('bword://')) {
        handled();
        const target = href.slice(8);
        if (target.startsWith('#')) scrollToAnchor(target.slice(1));
        else if (target) post({ type: 'lookup', word: decode(target.split('#')[0]) });
      } else if (lower.startsWith('http://') || lower.startsWith('https://') || lower.startsWith('mailto:')) {
        handled();
        post({ type: 'open', url: href });
      } else if (href.startsWith('#')) {
        event.preventDefault();
        if (href.length > 1) scrollToAnchor(href.slice(1));
      } else if (lower.startsWith('javascript:') || href === '') {
        // Leave script links to the dictionary's own handlers.
      } else {
        // A relative link inside an article names another headword (or a resource file).
        handled();
        if (!/\.(html?|css|js|png|jpe?g|gif|svg|webp|mp3|wav|ogg|spx|mp4)$/i.test(href))
          post({ type: 'lookup', word: decode(href.split('#')[0]) });
      }
    },
    true,
  );

  // Right-clicking selected text opens the app's menu (look up, copy, read aloud) instead of the
  // browser's; the app places it using the frame's position in the page.
  document.addEventListener('contextmenu', (event) => {
    if (!embedded || event.defaultPrevented) return;
    if (event.target instanceof Element && event.target.closest('input, textarea, [contenteditable]')) return;
    const text = String(window.getSelection() || '').trim();
    if (!text) return;
    event.preventDefault();
    post({ type: 'menu', text: text.slice(0, 4000), x: event.clientX, y: event.clientY });
  });

  document.addEventListener('dblclick', (event) => {
    if (event.target instanceof Element && event.target.closest('a, button, input, textarea')) return;
    const text = String(window.getSelection() || '').trim();
    if (text && text.length <= 64 && !/[\r\n]/.test(text)) post({ type: 'lookup', word: text });
  });

  // Ctrl +/−/0 zoom the whole window; the app applies it (browser shortcuts are off in WebView2).
  const zoomKeys = { '=': 'zoom-in', '+': 'zoom-in', '-': 'zoom-out', _: 'zoom-out', 0: 'zoom-reset' };
  document.addEventListener('keydown', (event) => {
    const editable = event.target instanceof Element && event.target.closest('input, textarea, select, [contenteditable]');
    if (event.key === 'Escape') {
      post({ type: 'key', key: 'escape' });
      return;
    }
    if (event.altKey && event.key === 'ArrowLeft') post({ type: 'key', key: 'back' });
    else if (event.altKey && event.key === 'ArrowRight') post({ type: 'key', key: 'forward' });
    else if (event.ctrlKey && ['k', 'l', 'f'].includes(event.key.toLowerCase())) post({ type: 'key', key: 'search' });
    else if (event.ctrlKey && zoomKeys[event.key]) post({ type: 'key', key: zoomKeys[event.key] });
    else if (!editable && !event.ctrlKey && !event.altKey && !event.metaKey && event.key.length === 1 && event.key.trim())
      post({ type: 'key', key: event.key });
    else return;
    event.preventDefault();
  });
  // Ctrl+wheel over an article zooms the window, a step per notch (touchpad pinches add up).
  let wheelZoom = 0;
  window.addEventListener(
    'wheel',
    (event) => {
      if (!event.ctrlKey) return;
      event.preventDefault();
      wheelZoom += event.deltaY;
      if (Math.abs(wheelZoom) < 100) return;
      post({ type: 'key', key: wheelZoom < 0 ? 'zoom-in' : 'zoom-out' });
      wheelZoom = 0;
    },
    { passive: false },
  );
  window.addEventListener('mouseup', (event) => {
    if (event.button === 3) post({ type: 'key', key: 'back' });
    if (event.button === 4) post({ type: 'key', key: 'forward' });
  });
})();
