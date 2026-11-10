import { $, call, escape, formatNumber, toast } from './bridge.ts';
import { hydrateIcons, icon } from './icons.ts';
import { avatar, effectiveTheme, formatNames, publish, state, subscribe, type Dictionary } from './state.ts';

type ImportResult = { cancelled?: boolean; dictionaries?: Dictionary[]; added?: number; skipped?: number; errors?: string[] };

const dialog = $<HTMLDialogElement>('library-dialog');
const list = $('dict-list');
const about = $('dict-about');
let confirming = '';
let awaitingReply = false;

function setDictionaries(dictionaries: Dictionary[]): void {
  state.dictionaries = dictionaries;
  publish('dictionaries');
}

function statusBadge(d: Dictionary): string {
  if (d.status === 'missing') return '<span class="badge danger">文件不存在</span>';
  if (d.status === 'stale') return '<span class="badge warning">文件已变化，需要重建索引</span>';
  return '';
}

function render(): void {
  if (!state.dictionaries.length) {
    list.innerHTML = `<li class="dict-empty">
        <div class="dict-empty-title">词典库是空的</div>
        <div>添加 .mdx 或 .ifo 文件，或选择一个存放词典的文件夹。</div>
      </li>`;
    return;
  }
  const count = state.dictionaries.length;
  list.innerHTML = state.dictionaries
    .map((d, i) => {
      const meta = [
        formatNames[d.format] ?? d.format,
        `${formatNumber(d.entries ?? 0)} 词条`,
        d.resources ? `${formatNumber(d.resources)} 个资源` : '',
      ]
        .filter(Boolean)
        .join(' · ');
      const actions =
        confirming === d.id
          ? `<span class="confirm-text">从词典库移除？</span>
             <button class="button danger small" type="button" data-action="remove-confirm">移除</button>
             <button class="button small" type="button" data-action="remove-cancel">取消</button>`
          : `<button class="icon-button small" type="button" data-action="up" title="上移" ${i === 0 ? 'disabled' : ''}>${icon('arrow-up')}</button>
             <button class="icon-button small" type="button" data-action="down" title="下移" ${i === count - 1 ? 'disabled' : ''}>${icon('arrow-down')}</button>
             <button class="icon-button small" type="button" data-action="about" title="简介" ${d.status === 'missing' ? 'disabled' : ''}>${icon('info')}</button>
             <button class="icon-button small" type="button" data-action="reindex" title="重建索引" ${d.status === 'missing' || state.importing ? 'disabled' : ''}>${icon('refresh')}</button>
             <button class="icon-button small" type="button" data-action="remove" title="移除" ${state.importing ? 'disabled' : ''}>${icon('trash')}</button>`;
      return `<li class="dict-row${d.enabled ? '' : ' disabled'}" data-id="${escape(d.id)}">
          ${avatar(d, 28)}
          <div class="dict-main">
            <div class="dict-name">${escape(d.name)} ${statusBadge(d)}</div>
            <div class="dict-meta">${meta}</div>
            <div class="dict-path" title="${escape(d.path)}">${escape(d.path)}</div>
          </div>
          <div class="dict-actions">${actions}</div>
          <label class="switch" title="${d.enabled ? '停用' : '启用'}"><input type="checkbox" data-action="toggle" ${d.enabled ? 'checked' : ''}/><span></span></label>
        </li>`;
    })
    .join('');
}

function setImporting(value: boolean): void {
  state.importing = value;
  for (const id of ['add-files', 'add-folder']) $<HTMLButtonElement>(id).disabled = value;
  $('import-progress').hidden = !value;
  if (value) {
    $('import-name').textContent = '正在准备…';
    $('import-percent').textContent = '';
    $('import-bar').style.width = '0%';
  }
  const button = $('open-library');
  button.classList.toggle('busy', value);
  render();
}

function summarize(result: ImportResult): void {
  if (result.cancelled) return;
  if (result.dictionaries) setDictionaries(result.dictionaries);
  const parts: string[] = [];
  if (result.added) parts.push(`已添加 ${result.added} 部词典`);
  if (result.skipped) parts.push(`${result.skipped} 部已在词典库中`);
  const errors = result.errors ?? [];
  if (errors.length) toast([...parts, `${errors.length} 个未能导入：${errors.slice(0, 2).join('；')}`].join('，'), 'error');
  else if (parts.length) toast(parts.join('，'), 'success');
}

export async function addDictionaries(kind: 'files' | 'folder'): Promise<void> {
  if (state.importing) return;
  awaitingReply = true;
  try {
    summarize(await call<ImportResult>(kind === 'files' ? 'dict.addFiles' : 'dict.addFolder'));
  } catch (error) {
    toast((error as Error).message, 'error');
  } finally {
    awaitingReply = false;
  }
}

function showAbout(d: Dictionary): void {
  list.hidden = true;
  about.hidden = false;
  $('about-meta').innerHTML = `${avatar(d, 36)}<div><div class="about-name">${escape(d.name)}</div>
      <div class="dict-meta">${formatNames[d.format] ?? d.format} · ${formatNumber(d.entries)} 词条${d.resources ? ` · ${formatNumber(d.resources)} 个资源` : ''}</div>
      <div class="dict-path">${escape(d.path)}</div></div>`;
  const theme = effectiveTheme() === 'dark' && state.settings.invertArticles ? 'dark' : 'light';
  $<HTMLIFrameElement>('about-frame').src = `https://dict.dr-dict.local/${encodeURIComponent(d.id)}/@about?theme=${theme}`;
}
function hideAbout(): void {
  about.hidden = true;
  list.hidden = false;
  $<HTMLIFrameElement>('about-frame').src = 'about:blank';
}

async function act(action: string, id: string, input?: HTMLInputElement): Promise<void> {
  const index = state.dictionaries.findIndex((d) => d.id === id);
  const dict = state.dictionaries[index];
  if (!dict) return;
  try {
    switch (action) {
      case 'toggle':
        setDictionaries(await call('dict.update', { id, patch: { enabled: input!.checked } }));
        break;
      case 'up':
      case 'down': {
        const ids = state.dictionaries.map((d) => d.id);
        const to = action === 'up' ? index - 1 : index + 1;
        if (to < 0 || to >= ids.length) return;
        [ids[index], ids[to]] = [ids[to], ids[index]];
        setDictionaries(await call('dict.reorder', { ids }));
        break;
      }
      case 'about':
        showAbout(dict);
        break;
      case 'remove':
        confirming = id;
        render();
        break;
      case 'remove-cancel':
        confirming = '';
        render();
        break;
      case 'remove-confirm':
        confirming = '';
        setDictionaries(await call('dict.remove', { id }));
        toast(`已移除“${dict.name}”`, 'success');
        break;
      case 'reindex':
        setImporting(true);
        try {
          setDictionaries(await call('dict.reindex', { id }));
          toast(`已重建“${dict.name}”的索引`, 'success');
        } finally {
          setImporting(false);
        }
        break;
    }
  } catch (error) {
    toast((error as Error).message, 'error');
    render();
  }
}

export function openLibrary(): void {
  confirming = '';
  hideAbout();
  render();
  if (!dialog.open) dialog.showModal();
}

export function initLibrary(on: (event: string, handler: (message: any) => void) => void): void {
  hydrateIcons(dialog);
  subscribe('dictionaries', () => {
    if (dialog.open) render();
  });
  $('add-files').addEventListener('click', () => void addDictionaries('files'));
  $('add-folder').addEventListener('click', () => void addDictionaries('folder'));
  $('about-back').addEventListener('click', hideAbout);
  dialog.addEventListener('click', (event) => {
    const target = event.target as HTMLElement;
    if (target === dialog || target.closest('[data-close]')) dialog.close();
  });
  dialog.addEventListener('close', hideAbout);
  list.addEventListener('click', (event) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('button[data-action]');
    const row = button?.closest<HTMLElement>('.dict-row');
    if (button && row) void act(button.dataset.action!, row.dataset.id!);
  });
  list.addEventListener('change', (event) => {
    const input = event.target as HTMLInputElement;
    const row = input.closest<HTMLElement>('.dict-row');
    if (input.dataset.action === 'toggle' && row) void act('toggle', row.dataset.id!, input);
  });
  on('import-start', () => setImporting(true));
  on('import-progress', (p) => {
    if (!state.importing) setImporting(true);
    const percent = Math.round((p.fraction ?? 0) * 100);
    const position = p.count > 1 ? `（${p.index + 1}/${p.count}）` : '';
    $('import-name').textContent = `正在建立索引：${p.name ?? ''}${position}`;
    $('import-percent').textContent = `${percent}%`;
    $('import-bar').style.width = `${percent}%`;
    $('library-count').textContent = `${percent}%`;
  });
  on('import-done', (result) => {
    setImporting(false);
    // Imports started from the command line have no pending request to report their outcome.
    if (!awaitingReply) summarize(result);
    else if (result.dictionaries) setDictionaries(result.dictionaries);
    else publish('dictionaries');
  });
}
