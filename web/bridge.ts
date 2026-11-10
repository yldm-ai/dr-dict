import { icon } from './icons.ts';

type Pending = { resolve: (value: any) => void; reject: (error: Error) => void; timer: number };
type Listener = (message: any) => void;

interface WebViewHost {
  postMessage(message: unknown): void;
  addEventListener(type: 'message', listener: (event: MessageEvent) => void): void;
}
const host = (window as unknown as { chrome?: { webview?: WebViewHost } }).chrome?.webview;

let sequence = 0;
const pending = new Map<number, Pending>();
const listeners = new Map<string, Set<Listener>>();

host?.addEventListener('message', ({ data }) => {
  if (data && typeof data.event === 'string') {
    for (const listener of listeners.get(data.event) ?? []) listener(data);
    return;
  }
  const item = pending.get(data?.id);
  if (!item) return;
  pending.delete(data.id);
  clearTimeout(item.timer);
  if (data.error) item.reject(new Error(data.error));
  else item.resolve(data.result);
});

export function call<T = any>(method: string, params: Record<string, unknown> = {}): Promise<T> {
  return new Promise((resolve, reject) => {
    if (!host) {
      reject(new Error('请通过 DR Dict 桌面程序打开界面。'));
      return;
    }
    const id = ++sequence;
    // Imports of large dictionaries can legitimately run for minutes.
    const timer = window.setTimeout(() => {
      pending.delete(id);
      reject(new Error('处理超时，请稍后重试。'));
    }, 30 * 60 * 1000);
    pending.set(id, { resolve, reject, timer });
    host.postMessage({ id, method, params });
  });
}

export function on(event: string, listener: Listener): void {
  if (!listeners.has(event)) listeners.set(event, new Set());
  listeners.get(event)!.add(listener);
}

export const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

export function escape(value: unknown): string {
  return String(value ?? '').replace(
    /[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]!,
  );
}

let toastTimer = 0;
export function toast(message: string, kind: 'info' | 'success' | 'error' = 'info'): void {
  const element = $('toast');
  clearTimeout(toastTimer);
  // A manual popover outside an open modal dialog is inert, so the toast moves into it.
  const modal = [...document.querySelectorAll('dialog')].find((d) => d.open) ?? null;
  const parent = modal ?? document.body;
  if (element.parentElement !== parent) {
    if (element.matches(':popover-open')) element.hidePopover();
    parent.append(element);
  }
  element.dataset.kind = kind;
  element.innerHTML = `${icon(kind === 'success' ? 'check' : kind === 'error' ? 'alert' : 'info')}<span></span>`;
  element.querySelector('span')!.textContent = message;
  if (!element.matches(':popover-open')) element.showPopover();
  toastTimer = window.setTimeout(
    () => {
      if (element.matches(':popover-open')) element.hidePopover();
    },
    kind === 'error' ? 6000 : 2800,
  );
}

export function formatNumber(value: number): string {
  return new Intl.NumberFormat('zh-CN').format(value);
}
