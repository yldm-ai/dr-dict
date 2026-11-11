// The dictionary selector at the right of the search box: search all enabled dictionaries or a
// chosen few. The choice is saved as the "scope" setting (an empty list means all).
import { $, call, escape, toast } from './bridge.ts';
import { hydrateIcons } from './icons.ts';
import { activeDictionaries, avatar, publish, state, subscribe } from './state.ts';

const button = $<HTMLButtonElement>('scope-button');
const menu = $('scope-menu');
const countBadge = $('scope-count');
let onChange: () => void = () => {};

function searchable() {
  return activeDictionaries().filter((d) => d.enabled);
}

/** Dictionary ids to send with lookups and suggestions; empty means every enabled dictionary. */
export function scopeIds(): string[] {
  const chosen = new Set(state.settings.scope ?? []);
  const ids = searchable()
    .map((d) => d.id)
    .filter((id) => chosen.has(id));
  // A scope whose dictionaries were all disabled or removed falls back to all.
  return ids.length && ids.length < searchable().length ? ids : [];
}

function renderButton(): void {
  const ids = scopeIds();
  const limited = ids.length > 0;
  button.classList.toggle('limited', limited);
  countBadge.hidden = !limited;
  countBadge.textContent = limited ? String(ids.length) : '';
  const names = searchable()
    .filter((d) => ids.includes(d.id))
    .map((d) => d.name);
  const tip = limited ? `查询范围：${names.join('、')}` : '查询范围：全部词典';
  button.title = tip; // the tooltip module turns titles into its own hover tips
  button.setAttribute('aria-label', tip);
}

function renderMenu(): void {
  const dictionaries = searchable();
  const ids = scopeIds();
  const all = ids.length === 0;
  if (!dictionaries.length) {
    menu.innerHTML = `<div class="scope-empty">没有已启用的词典</div>`;
    return;
  }
  menu.innerHTML =
    `<label class="scope-item scope-all"><input type="checkbox" data-scope="all" ${all ? 'checked' : ''}/><span>全部词典</span></label>
     <div class="scope-divider"></div>` +
    dictionaries
      .map(
        (d) => `<label class="scope-item" title="${escape(d.name)}">
          <input type="checkbox" data-scope="${escape(d.id)}" ${all || ids.includes(d.id) ? 'checked' : ''}/>
          ${avatar(d, 18)}<span class="scope-name">${escape(d.name)}</span>
          <button class="scope-only" type="button" data-only="${escape(d.id)}">仅此本</button>
        </label>`,
      )
      .join('');
  hydrateIcons(menu);
}

async function save(ids: string[]): Promise<void> {
  // Choosing every dictionary is the same as "all", which keeps newly added ones included.
  const scope = ids.length >= searchable().length ? [] : ids;
  state.settings = { ...state.settings, scope };
  renderButton();
  renderMenu();
  try {
    state.settings = await call('settings.save', { scope });
    publish('settings');
  } catch (error) {
    toast((error as Error).message, 'error');
  }
  onChange();
}

function open(): void {
  renderMenu();
  menu.hidden = false;
  button.setAttribute('aria-expanded', 'true');
}
function close(): void {
  menu.hidden = true;
  button.setAttribute('aria-expanded', 'false');
}

export function initScope(handlers: { onChange: () => void }): void {
  onChange = handlers.onChange;
  button.addEventListener('click', () => (menu.hidden ? open() : close()));
  menu.addEventListener('click', (event) => {
    const only = (event.target as HTMLElement).closest<HTMLElement>('[data-only]');
    if (!only) return;
    event.preventDefault();
    void save([only.dataset.only!]);
  });
  menu.addEventListener('change', (event) => {
    const input = event.target as HTMLInputElement;
    const key = input.dataset.scope;
    if (!key) return;
    if (key === 'all') {
      void save(input.checked ? [] : scopeIds());
      return;
    }
    const current = scopeIds().length ? scopeIds() : searchable().map((d) => d.id);
    const next = input.checked ? [...current, key] : current.filter((id) => id !== key);
    if (!next.length) {
      // At least one dictionary is searched; unticking the last one is undone.
      input.checked = true;
      toast('至少保留一部词典', 'error');
      return;
    }
    // Keep the library order.
    void save(searchable().map((d) => d.id).filter((id) => next.includes(id)));
  });
  document.addEventListener('mousedown', (event) => {
    if (!menu.hidden && !(event.target as HTMLElement).closest('#scope-menu, #scope-button')) close();
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && !menu.hidden) {
      event.stopPropagation();
      close();
      button.focus();
    }
  }, true);
  subscribe('dictionaries', () => {
    renderButton();
    if (!menu.hidden) renderMenu();
  });
  subscribe('settings', renderButton);
  renderButton();
}
