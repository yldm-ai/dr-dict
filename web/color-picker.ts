// Compact colour flyout after DR Reader's (web/color-picker.ts): preset swatches, a
// saturation/brightness field, a hue strip and a hex field. It lives inside host (the settings
// dialog, which is in the top layer) as a fixed-position flyout.

type Hsv = { h: number; s: number; v: number };

function toHsv(color: string): Hsv {
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(color.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b);
  const d = max - Math.min(r, g, b);
  const h = !d ? 0 : max === r ? ((g - b) / d + 6) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return { h: h * 60, s: max ? d / max : 0, v: max };
}
function fromHsv({ h, s, v }: Hsv): string {
  const f = (n: number) => {
    const k = (n + h / 60) % 6;
    return v - v * s * Math.max(0, Math.min(k, 4 - k, 1));
  };
  return `#${[5, 3, 1].map((n) => Math.round(f(n) * 255).toString(16).padStart(2, '0')).join('')}`;
}
const clamp = (value: number) => Math.max(0, Math.min(1, value));

export class ColorPicker {
  private readonly root: HTMLDivElement;
  private readonly presets: HTMLDivElement;
  private readonly area: HTMLDivElement;
  private readonly areaHandle: HTMLDivElement;
  private readonly hue: HTMLDivElement;
  private readonly hueHandle: HTMLDivElement;
  private readonly hex: HTMLInputElement;
  private readonly preview: HTMLSpanElement;
  private readonly title: HTMLDivElement;
  private hsv: Hsv = { h: 45, s: 0.13, v: 0.98 };
  private onPick?: (color: string) => void;
  private anchor?: HTMLElement;

  constructor(host: HTMLElement = document.body) {
    const root = document.createElement('div');
    root.className = 'color-picker';
    root.hidden = true;
    root.setAttribute('role', 'dialog');
    root.innerHTML = `<div class="cp-title"></div>
      <div class="cp-presets"></div>
      <div class="cp-area" tabindex="0" role="slider" aria-label="饱和度与亮度"><div class="cp-area-handle"></div></div>
      <div class="cp-hue" tabindex="0" role="slider" aria-label="色相" aria-valuemin="0" aria-valuemax="360"><div class="cp-hue-handle"></div></div>
      <div class="cp-footer"><span class="cp-preview"></span><input class="cp-hex" maxlength="7" spellcheck="false" aria-label="十六进制颜色">
        <button type="button" class="button small ghost cp-cancel">取消</button><button type="button" class="button small primary cp-apply">使用</button></div>`;
    host.append(root);
    this.root = root;
    this.title = root.querySelector('.cp-title')!;
    this.presets = root.querySelector('.cp-presets')!;
    this.area = root.querySelector('.cp-area')!;
    this.areaHandle = root.querySelector('.cp-area-handle')!;
    this.hue = root.querySelector('.cp-hue')!;
    this.hueHandle = root.querySelector('.cp-hue-handle')!;
    this.hex = root.querySelector('.cp-hex')!;
    this.preview = root.querySelector('.cp-preview')!;
    this.drag(this.area, (x, y) => {
      this.hsv.s = x;
      this.hsv.v = 1 - y;
      this.render();
    });
    this.drag(this.hue, (x) => {
      this.hsv.h = x * 360;
      this.render();
    });
    this.keys(this.area, (dx, dy) => {
      this.hsv.s = clamp(this.hsv.s + dx);
      this.hsv.v = clamp(this.hsv.v - dy);
    });
    this.keys(this.hue, (dx) => {
      this.hsv.h = (this.hsv.h + dx * 360 + 360) % 360;
    });
    this.presets.addEventListener('click', (event) => {
      const button = (event.target as HTMLElement).closest<HTMLElement>('[data-color]');
      if (!button) return;
      this.hsv = toHsv(button.dataset.color!);
      this.render();
    });
    this.hex.addEventListener('input', () => {
      const value = this.hex.value.trim().replace(/^#?/, '#');
      if (/^#[0-9a-fA-F]{6}$/.test(value)) {
        this.hsv = toHsv(value.toLowerCase());
        this.render(false);
      }
    });
    this.hex.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        this.apply();
      }
    });
    root.querySelector('.cp-cancel')!.addEventListener('click', () => this.close());
    root.querySelector('.cp-apply')!.addEventListener('click', () => this.apply());
    root.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') {
        event.preventDefault();
        event.stopPropagation();
        this.close();
      }
    });
    document.addEventListener(
      'pointerdown',
      (event) => {
        const target = event.target as Node;
        if (this.isOpen && !root.contains(target) && !this.anchor?.contains(target)) this.close(false);
      },
      true,
    );
  }

  get isOpen(): boolean {
    return !this.root.hidden;
  }

  open(anchor: HTMLElement, options: { title: string; color: string; presets: string[]; onPick: (color: string) => void }): void {
    this.anchor = anchor;
    this.onPick = options.onPick;
    this.title.textContent = options.title;
    this.presets.innerHTML = options.presets
      .map((c) => `<button type="button" class="cp-preset" data-color="${c}" style="--swatch:${c}" title="${c}" aria-label="${c}"></button>`)
      .join('');
    this.hsv = toHsv(/^#[0-9a-f]{6}$/i.test(options.color) ? options.color.toLowerCase() : '#ffffff');
    this.root.hidden = false;
    this.render();
    const rect = anchor.getBoundingClientRect();
    const box = this.root.getBoundingClientRect();
    const below = rect.bottom + 6 + box.height <= innerHeight - 8;
    this.root.style.left = `${Math.round(Math.max(8, Math.min(rect.left, innerWidth - box.width - 8)))}px`;
    this.root.style.top = `${Math.round(below ? rect.bottom + 6 : Math.max(8, rect.top - 6 - box.height))}px`;
    this.area.focus({ preventScroll: true });
  }

  close(returnFocus = true): void {
    if (!this.isOpen) return;
    this.root.hidden = true;
    if (returnFocus) this.anchor?.focus({ preventScroll: true });
  }

  private apply(): void {
    const color = fromHsv(this.hsv);
    this.close();
    this.onPick?.(color);
  }

  private render(syncField = true): void {
    const color = fromHsv(this.hsv);
    this.area.style.setProperty('--hue', fromHsv({ h: this.hsv.h, s: 1, v: 1 }));
    this.areaHandle.style.left = `${this.hsv.s * 100}%`;
    this.areaHandle.style.top = `${(1 - this.hsv.v) * 100}%`;
    this.areaHandle.style.background = color;
    this.hueHandle.style.left = `${(this.hsv.h / 360) * 100}%`;
    this.hue.setAttribute('aria-valuenow', String(Math.round(this.hsv.h)));
    this.preview.style.background = color;
    if (syncField) this.hex.value = color;
    for (const button of this.presets.querySelectorAll<HTMLElement>('.cp-preset'))
      button.classList.toggle('selected', button.dataset.color === color);
  }

  /** Pointer drags report positions inside the element as 0..1 fractions. */
  private drag(element: HTMLElement, move: (x: number, y: number) => void): void {
    const update = (event: PointerEvent) => {
      const rect = element.getBoundingClientRect();
      move(clamp((event.clientX - rect.left) / rect.width), clamp((event.clientY - rect.top) / rect.height));
    };
    element.addEventListener('pointerdown', (event) => {
      if (event.button !== 0) return;
      event.preventDefault();
      element.setPointerCapture(event.pointerId);
      element.focus({ preventScroll: true });
      update(event);
    });
    element.addEventListener('pointermove', (event) => {
      if (element.hasPointerCapture(event.pointerId)) update(event);
    });
  }

  private keys(element: HTMLElement, step: (dx: number, dy: number) => void): void {
    element.addEventListener('keydown', (event) => {
      const delta = event.shiftKey ? 0.1 : 0.02;
      const moves: Record<string, [number, number]> = {
        ArrowLeft: [-delta, 0],
        ArrowRight: [delta, 0],
        ArrowUp: [0, -delta],
        ArrowDown: [0, delta],
      };
      const move = moves[event.key];
      if (!move) {
        if (event.key === 'Enter') {
          event.preventDefault();
          this.apply();
        }
        return;
      }
      event.preventDefault();
      step(...move);
      this.render();
    });
  }
}
