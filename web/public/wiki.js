// Wiktionary article frames: collapsible boxes (translations, phrasebook, inflection tables) open
// and close from their header, which the site does with its own scripts. frame.js notices the
// height change and resizes the frame.
document.addEventListener('DOMContentLoaded', () => {
  // GoldenDict-ng keeps MediaWiki markup and supplies scoped article styles (mediawiki.cc's
  // requestFinished and stylesheets/article-style.css). Independently adapt the retained floats
  // to our narrower iframe: adjacent illustration boxes form a wrapping row, never a text gutter.
  for (const content of document.querySelectorAll('.dr-wiki .mw-parser-output')) {
    let row = null;
    for (const child of [...content.children]) {
      if (['STYLE', 'LINK', 'SCRIPT'].includes(child.tagName)) continue;
      const style = getComputedStyle(child);
      if (style.display === 'none') continue;
      if (style.cssFloat === 'none') {
        row = null;
        continue;
      }
      if (!row) {
        row = document.createElement('div');
        row.className = 'dr-wiki-media';
        child.before(row);
      }
      row.append(child);
    }
  }
  // Wrap only outer tables; nesting wrappers inside cells changes a template's own layout.
  for (const table of document.querySelectorAll('.dr-wiki table')) {
    if (table.parentElement.closest('table')) continue;
    const viewport = document.createElement('div');
    viewport.className = 'dr-wiki-table';
    viewport.tabIndex = 0;
    viewport.setAttribute('role', 'region');
    viewport.setAttribute('aria-label', '词条表格');
    table.before(viewport);
    viewport.append(table);
  }
});

document.addEventListener('click', (event) => {
  const head = event.target instanceof Element ? event.target.closest('.NavHead') : null;
  if (!head || event.target.closest('a')) return;
  head.parentElement?.classList.toggle('dr-open');
});
