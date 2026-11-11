// 原 / 简 / 繁 at the right of each dictionary header, shared by the main window (articles.ts) and
// the Ctrl+C+C popup (popup.ts). The choice is kept per dictionary in localStorage, which both
// pages share (same origin); it is read afresh each time, so a choice made in one page applies in
// the other.
import { hydrateIcons } from './icons.ts';
import { state, traditionalNames, type TraditionalVariant } from './state.ts';

// How a dictionary's articles are written out: as written ("o"), simplified ("s") or traditional
// ("t"). The traditional standard picked for a dictionary is kept on its own, so it survives
// switching to 原 or 简 and back; without one, 繁 uses the default from the settings.
export type ScriptChoice = { script: 'o' | 's' | 't'; variant?: TraditionalVariant };

function load<T>(key: string): Record<string, T> {
  try {
    return JSON.parse(localStorage.getItem(key) ?? '{}') ?? {};
  } catch {
    return {};
  }
}

export function scriptOf(id: string): ScriptChoice {
  const value = state.chineseAvailable ? load<string>('scripts')[id] : undefined;
  const standard = load<TraditionalVariant>('traditional')[id];
  const variant = standard in traditionalNames ? standard : undefined;
  return { script: value === 's' || value === 't' ? value : 'o', variant };
}
function setScript(id: string, choice: ScriptChoice): void {
  const scripts = load<string>('scripts');
  const standards = load<TraditionalVariant>('traditional');
  if (choice.script === 'o') delete scripts[id];
  else scripts[id] = choice.script;
  if (choice.variant) standards[id] = choice.variant;
  else delete standards[id];
  localStorage.setItem('scripts', JSON.stringify(scripts));
  localStorage.setItem('traditional', JSON.stringify(standards));
}
function traditionalOf(choice: ScriptChoice): TraditionalVariant {
  return choice.variant ?? state.settings.traditionalVariant ?? 't';
}

/** The `&script=…` part of an article URL for a dictionary. */
export function scriptQuery(id: string): string {
  const choice = scriptOf(id);
  return choice.script === 's' ? '&script=s' : choice.script === 't' ? `&script=t&variant=${traditionalOf(choice)}` : '';
}

/** The switch's markup; the arrow beside 繁 picks the traditional standard. Empty without OpenCC. */
export function scriptSwitch(id: string): string {
  if (!state.chineseAvailable) return '';
  const choice = scriptOf(id);
  const button = (script: ScriptChoice['script'], label: string, tip: string) =>
    `<button class="script-option" type="button" data-script="${script}" aria-pressed="${choice.script === script}"
       title="${tip}">${label}</button>`;
  return `<div class="script-switch" role="group" aria-label="释义写法">
      ${button('o', '原', '词典原文')}${button('s', '简', '转成简体')}${button('t', '繁', `转成${traditionalNames[traditionalOf(choice)]}`)}
      <button class="script-more" type="button" aria-haspopup="menu" aria-expanded="false" title="选择繁体标准">
        <span data-icon="chevron-down"></span>
      </button>
    </div>`;
}

/**
 * Wires the switches inside `container` (whose `.entry` sections carry data-id). `reload` is called
 * with a section after its choice changed, to load its article again; `scroller` closes the menu
 * when it scrolls.
 */
export function initScriptSwitch(container: HTMLElement, scroller: HTMLElement, reload: (section: HTMLElement) => void): void {
  // One menu for all headers, placed under the arrow that opened it (fixed, so later sticky
  // headers cannot cover it).
  const menu = document.createElement('div');
  menu.className = 'script-menu';
  menu.setAttribute('role', 'menu');
  menu.hidden = true;
  document.body.append(menu);
  let menuFor: HTMLButtonElement | null = null;

  function open(arrow: HTMLButtonElement): void {
    const id = arrow.closest<HTMLElement>('.entry')?.dataset.id;
    if (!id) return;
    const choice = scriptOf(id);
    const active = choice.script === 't' ? traditionalOf(choice) : null;
    menu.innerHTML = (Object.keys(traditionalNames) as TraditionalVariant[])
      .map(
        (variant) => `<button class="script-menu-item" type="button" role="menuitemradio" data-variant="${variant}"
            aria-checked="${active === variant}">
            <span class="script-menu-check" data-icon="check"></span>
            <span class="script-menu-name">${traditionalNames[variant]}</span>
            ${variant === state.settings.traditionalVariant ? '<span class="script-menu-default">默认</span>' : ''}
          </button>`,
      )
      .join('');
    hydrateIcons(menu);
    menu.hidden = false;
    const rect = arrow.getBoundingClientRect();
    const width = menu.offsetWidth;
    menu.style.left = `${Math.max(8, Math.min(rect.right - width, innerWidth - width - 8))}px`;
    // Below the arrow, or above it when the window is too short (the popup is small).
    const below = rect.bottom + 4;
    menu.style.top = `${below + menu.offsetHeight > innerHeight - 8 ? Math.max(8, rect.top - 4 - menu.offsetHeight) : below}px`;
    arrow.setAttribute('aria-expanded', 'true');
    menuFor = arrow;
  }
  function close(): void {
    if (menu.hidden) return;
    menu.hidden = true;
    menuFor?.setAttribute('aria-expanded', 'false');
    menuFor = null;
  }
  /** Shows one dictionary's article as written, simplified or traditional. */
  function apply(section: HTMLElement, choice: ScriptChoice): void {
    const id = section.dataset.id!;
    setScript(id, choice);
    const switcher = section.querySelector('.script-switch');
    if (switcher) {
      switcher.outerHTML = scriptSwitch(id);
      hydrateIcons(section.querySelector('.entry-header')!);
    }
    section.classList.remove('collapsed');
    section.querySelector('.entry-toggle')?.setAttribute('aria-expanded', 'true');
    reload(section);
  }

  // Capture, so the container's own click handler (folding on the header) never sees these.
  container.addEventListener(
    'click',
    (event) => {
      const target = event.target as HTMLElement;
      const option = target.closest<HTMLButtonElement>('.script-option');
      const more = target.closest<HTMLButtonElement>('.script-more');
      if (!option && !more) return;
      event.stopPropagation();
      if (option) {
        // The standard picked for this dictionary stays, whichever button is pressed.
        const section = option.closest<HTMLElement>('.entry')!;
        apply(section, { script: option.dataset.script as ScriptChoice['script'], variant: scriptOf(section.dataset.id!).variant });
      } else if (menuFor === more) close();
      else {
        close();
        open(more!);
      }
    },
    true,
  );
  menu.addEventListener('click', (event) => {
    const item = (event.target as HTMLElement).closest<HTMLElement>('[data-variant]');
    const section = menuFor?.closest<HTMLElement>('.entry');
    if (!item || !section) return;
    const variant = item.dataset.variant as TraditionalVariant;
    close();
    // Picking the default standard follows the setting from then on.
    apply(section, { script: 't', variant: variant === state.settings.traditionalVariant ? undefined : variant });
  });
  document.addEventListener('mousedown', (event) => {
    if (!(event.target as HTMLElement).closest('.script-menu, .script-more')) close();
  });
  document.addEventListener(
    'keydown',
    (event) => {
      if (event.key === 'Escape' && menuFor) {
        event.stopPropagation();
        const arrow = menuFor;
        close();
        arrow.focus();
      }
    },
    true,
  );
  scroller.addEventListener('scroll', close, { passive: true });
  window.addEventListener('resize', close);
  window.addEventListener('blur', close); // e.g. a click into an article frame
}
