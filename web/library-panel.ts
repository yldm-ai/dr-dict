import { $, call, escape, formatNumber, toast } from './bridge.ts';
import { hydrateIcons, icon } from './icons.ts';
import {
  activeDictionaries,
  avatar,
  effectiveTheme,
  formatNames,
  publish,
  state,
  subscribe,
  type Dictionary,
} from './state.ts';

type ImportResult = {
  cancelled?: boolean;
  dictionaries?: Dictionary[];
  added?: number;
  skipped?: number;
  errors?: string[];
  folders?: number;
};
type Folder = { path: string; exists: boolean };

const dialog = $<HTMLDialogElement>('settings-dialog');
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

// The removed group starts collapsed each time the library tab is shown.
let showRemoved = false;

function removedRow(d: Dictionary): string {
  const meta = [formatNames[d.format] ?? d.format, `${formatNumber(d.entries ?? 0)} 词条`].join(' · ');
  return `<li class="dict-row removed" data-id="${escape(d.id)}">
      ${avatar(d, 28)}
      <div class="dict-main">
        <div class="dict-name">${escape(d.name)} ${d.fileMissing ? '<span class="badge danger">文件不存在</span>' : ''}</div>
        <div class="dict-meta">${meta}</div>
        <div class="dict-path" title="${escape(d.path)}">${escape(d.path)}</div>
      </div>
      <div class="dict-actions">
        ${
          d.fileMissing
            ? `<button class="button small" type="button" data-action="forget" title="文件已不存在，从列表中清除">清除</button>`
            : `<button class="button small" type="button" data-action="restore" title="重新建立索引并启用" ${state.importing ? 'disabled' : ''}>恢复</button>`
        }
      </div>
    </li>`;
}

function render(): void {
  const active = activeDictionaries();
  const removed = state.dictionaries.filter((d) => d.removed);
  const removedGroup = removed.length
    ? `<li class="dict-group">
         <button class="dict-group-toggle" type="button" data-action="toggle-removed" aria-expanded="${showRemoved}">
           ${icon('chevron-down')}<span>已移除</span><span class="count">${removed.length}</span>
         </button>
       </li>${showRemoved ? removed.map(removedRow).join('') : ''}`
    : '';
  if (!active.length) {
    list.innerHTML = `<li class="dict-empty">
        <div class="dict-empty-title">词典库是空的</div>
        <div>添加 .mdx 或 .ifo 文件，或选择一个存放词典的文件夹。</div>
      </li>${removedGroup}`;
    return;
  }
  const count = active.length;
  list.innerHTML =
    active
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
          ? `<span class="confirm-text">移到“已移除”？</span>
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
    .join('') + removedGroup;
}

function setImporting(value: boolean): void {
  state.importing = value;
  for (const id of ['add-files', 'add-folder', 'refresh-library']) $<HTMLButtonElement>(id).disabled = value;
  $('import-progress').hidden = !value;
  if (value) {
    $('import-name').textContent = '正在准备…';
    $('import-percent').textContent = '';
    $('import-bar').style.width = '0%';
  }
  const badge = $('settings-badge');
  badge.hidden = !value;
  badge.textContent = '';
  $('open-settings').classList.toggle('busy', value);
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
  if (kind === 'folder') void loadFolders();
}

// Folders added with "添加文件夹" are remembered; refresh scans them again.
const folderList = $('folder-list');
function renderFolders(folders: Folder[]): void {
  folderList.hidden = !folders.length;
  folderList.innerHTML =
    `<span class="folder-label">已添加的文件夹</span>` +
    folders
      .map(
        (f) => `<span class="folder-chip${f.exists ? '' : ' missing'}" title="${escape(f.path)}${f.exists ? '' : '（不存在）'}">
          ${icon('folder')}<span class="folder-path">${escape(f.path)}</span>
          <button class="folder-remove" type="button" data-folder="${escape(f.path)}" title="不再扫描此文件夹" aria-label="不再扫描此文件夹">${icon('x')}</button>
        </span>`,
      )
      .join('');
}
async function loadFolders(): Promise<void> {
  try {
    renderFolders(await call<Folder[]>('dict.folders'));
  } catch {
    renderFolders([]);
  }
}

// Scans the remembered folders for new dictionaries, then rebuilds the index of those whose
// files changed, like GoldenDict's "Rescan now".
let refreshing = false;
async function refreshLibrary(): Promise<void> {
  if (state.importing || refreshing) return;
  refreshing = true;
  // A check of unchanged files takes milliseconds; the icon still makes one full turn so the
  // click visibly did something.
  const button = $('refresh-library');
  button.classList.add('spinning');
  const started = performance.now();
  try {
    // The rescan reports through the import events too; this request carries the outcome.
    awaitingReply = true;
    let scan: ImportResult;
    try {
      scan = await call<ImportResult>('dict.rescan');
    } finally {
      awaitingReply = false;
    }
    if (scan.dictionaries) setDictionaries(scan.dictionaries);
    void loadFolders();
    const stale = state.dictionaries.filter((d) => d.status === 'stale');
    const missing = state.dictionaries.filter((d) => d.status === 'missing').length;
    if (stale.length) {
      setImporting(true);
      try {
        for (const d of stale) setDictionaries(await call<Dictionary[]>('dict.reindex', { id: d.id }));
      } finally {
        setImporting(false);
      }
    }
    await new Promise((resolve) => setTimeout(resolve, Math.max(0, 600 - (performance.now() - started))));
    const parts: string[] = [];
    if (scan.added) parts.push(`新增 ${scan.added} 部词典`);
    if (stale.length) parts.push(`已重建 ${stale.length} 部词典的索引`);
    if (!parts.length) parts.push(scan.folders ? '没有新词典，词典都是最新的' : '词典都是最新的');
    if (missing) parts.push(`${missing} 部词典的文件不存在`);
    const errors = scan.errors ?? [];
    if (errors.length) parts.push(`${errors.length} 个未能导入：${errors.slice(0, 2).join('；')}`);
    toast(parts.join('，'), missing || errors.length ? 'error' : 'success');
  } catch (error) {
    toast((error as Error).message, 'error');
  } finally {
    button.classList.remove('spinning');
    refreshing = false;
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
        // Order only among the active dictionaries; the removed ones keep their place after them.
        const ids = activeDictionaries().map((d) => d.id);
        const from = ids.indexOf(id);
        const to = action === 'up' ? from - 1 : from + 1;
        if (from < 0 || to < 0 || to >= ids.length) return;
        [ids[from], ids[to]] = [ids[to], ids[from]];
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
        toast(`已将“${dict.name}”移到“已移除”，可随时恢复`, 'success');
        break;
      case 'restore':
        setImporting(true);
        try {
          setDictionaries(await call('dict.restore', { id }));
          toast(`已恢复“${dict.name}”`, 'success');
        } finally {
          setImporting(false);
        }
        break;
      case 'forget':
        setDictionaries(await call('dict.forget', { id }));
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

// Called when the settings dialog shows the library tab or closes.
export function resetLibrary(): void {
  confirming = '';
  showRemoved = false;
  hideAbout();
  render();
  void loadFolders();
}

export function initLibrary(on: (event: string, handler: (message: any) => void) => void): void {
  hydrateIcons(list.parentElement!);
  subscribe('dictionaries', () => {
    if (dialog.open) render();
  });
  $('add-files').addEventListener('click', () => void addDictionaries('files'));
  $('add-folder').addEventListener('click', () => void addDictionaries('folder'));
  $('refresh-library').addEventListener('click', () => void refreshLibrary());
  folderList.addEventListener('click', async (event) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('button[data-folder]');
    if (!button) return;
    try {
      // The dictionaries found in it stay; only future refreshes skip the folder.
      renderFolders(await call<Folder[]>('dict.removeFolder', { path: button.dataset.folder }));
      toast('已不再扫描该文件夹，其中的词典仍保留在词典库中', 'success');
    } catch (error) {
      toast((error as Error).message, 'error');
    }
  });
  $('about-back').addEventListener('click', hideAbout);
  dialog.addEventListener('close', hideAbout);
  list.addEventListener('click', (event) => {
    const button = (event.target as HTMLElement).closest<HTMLButtonElement>('button[data-action]');
    if (button?.dataset.action === 'toggle-removed') {
      showRemoved = !showRemoved;
      render();
      return;
    }
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
    $('settings-badge').textContent = `${percent}%`;
  });
  on('import-done', (result) => {
    setImporting(false);
    // Imports started from the command line have no pending request to report their outcome.
    if (!awaitingReply) summarize(result);
    else if (result.dictionaries) setDictionaries(result.dictionaries);
    else publish('dictionaries');
  });
}
