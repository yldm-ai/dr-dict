// Wiktionary article frames: collapsible boxes (translations, phrasebook, inflection tables) open
// and close from their header, which the site does with its own scripts. frame.js notices the
// height change and resizes the frame.
document.addEventListener('click', (event) => {
  const head = event.target instanceof Element ? event.target.closest('.NavHead') : null;
  if (!head || event.target.closest('a')) return;
  head.parentElement?.classList.toggle('dr-open');
});
