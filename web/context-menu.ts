// The menu for selected text, in article frames and in the app's own page: look the selection
// up, copy it, or read it aloud. It replaces the browser's menu there (which offered little more
// than Copy); text fields keep the system menu for cut and paste.
import { escape, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { speak } from './speech.ts';

const menu = document.createElement('div');
menu.className = 'context-menu';
menu.setAttribute('role', 'menu');
menu.hidden = true;
document.body.append(menu);

let selected = '';
let handlers: { lookup: (word: string) => void } = { lookup: () => {} };
let copyInFrame: (() => void) | null = null;

const hasHan = (text: string) => /[㐀-鿿豈-﫿]/.test(text);
/** The selection as one line, for lookups and the menu label. */
const oneLine = (text: string) => text.replace(/\s+/g, ' ').trim();

export function hideContextMenu(): void {
  menu.hidden = true;
  copyInFrame = null;
}

/**
 * Opens the menu at (x, y) in page coordinates for `text`. `copy` copies in the document that
 * holds the selection when the clipboard API is unavailable (an article frame).
 */
export function showContextMenu(x: number, y: number, text: string, copy?: () => void): void {
  selected = text;
  copyInFrame = copy ?? null;
  const word = oneLine(text);
  const label = word.length > 18 ? `${word.slice(0, 18)}…` : word;
  const lookupable = word.length > 0 && word.length <= 200;
  menu.innerHTML = `
    ${
      lookupable
        ? `<button class="context-item" type="button" role="menuitem" data-command="lookup">
             <span data-icon="search"></span><span class="context-label">查询“<span class="context-word">${escape(label)}</span>”</span>
           </button>`
        : ''
    }
    <button class="context-item" type="button" role="menuitem" data-command="copy">
      <span data-icon="copy"></span><span class="context-label">复制</span><kbd class="context-key">Ctrl C</kbd>
    </button>
    <button class="context-item" type="button" role="menuitem" data-command="speak">
      <span data-icon="sound"></span><span class="context-label">朗读</span>
    </button>`;
  hydrateIcons(menu);
  menu.hidden = false;
  // Keep the menu inside the window, flipping above or left of the pointer near an edge.
  const { offsetWidth: width, offsetHeight: height } = menu;
  const left = x + width + 8 > innerWidth ? Math.max(8, x - width) : x;
  const top = y + height + 8 > innerHeight ? Math.max(8, y - height) : y;
  menu.style.left = `${left}px`;
  menu.style.top = `${top}px`;
  menu.querySelector<HTMLButtonElement>('.context-item')?.focus({ preventScroll: true });
}

async function run(command: string): Promise<void> {
  const text = selected;
  const copy = copyInFrame;
  hideContextMenu();
  if (command === 'lookup') handlers.lookup(oneLine(text));
  else if (command === 'copy') {
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      if (copy) copy();
      else document.execCommand('copy');
    }
  } else if (command === 'speak') {
    if (!speak(oneLine(text), hasHan(text) ? 'zh-CN' : 'en-US')) toast('没有可用的本机朗读语音', 'error');
  }
}

export function initContextMenu(options: { lookup: (word: string) => void }): void {
  handlers = options;
  menu.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>('[data-command]');
    if (item) void run(item.dataset.command!);
  });
  menu.addEventListener('contextmenu', (event) => event.preventDefault());
  // The app's own page: selected text outside text fields.
  document.addEventListener('contextmenu', (event) => {
    const target = event.target as HTMLElement;
    if (target.closest('input, textarea, [contenteditable], .context-menu')) return;
    const text = String(window.getSelection() ?? '').trim();
    if (!text) return;
    event.preventDefault();
    showContextMenu(event.clientX, event.clientY, text);
  });
  document.addEventListener('mousedown', (event) => {
    if (!menu.hidden && !(event.target as HTMLElement).closest('.context-menu')) hideContextMenu();
  });
  document.addEventListener(
    'keydown',
    (event) => {
      if (menu.hidden) return;
      const items = [...menu.querySelectorAll<HTMLButtonElement>('.context-item')];
      const index = items.indexOf(document.activeElement as HTMLButtonElement);
      if (event.key === 'Escape') hideContextMenu();
      else if (event.key === 'ArrowDown') items[(index + 1) % items.length]?.focus();
      else if (event.key === 'ArrowUp') items[(index - 1 + items.length) % items.length]?.focus();
      else return;
      event.preventDefault();
      event.stopPropagation();
    },
    true,
  );
  for (const type of ['blur', 'resize']) window.addEventListener(type, hideContextMenu);
  document.addEventListener('scroll', hideContextMenu, { capture: true, passive: true });
}
