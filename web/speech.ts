// Reads dictionary text aloud for pronunciation buttons that carry no recording (see frame.js).
// Uses the Windows voices exposed through the Web Speech API, so it works offline.

import { state } from './state.ts';

const synth = window.speechSynthesis as SpeechSynthesis | undefined;
// Voices load asynchronously; asking once at start-up has them ready by the first click.
synth?.getVoices();

function pickVoice(lang: string): SpeechSynthesisVoice | undefined {
  const voices = synth?.getVoices() ?? [];
  const exact = voices.filter((v) => v.lang.toLowerCase() === lang.toLowerCase());
  // A requested British accent falls back to any English voice (Windows often ships US only).
  const family = voices.filter((v) => v.lang.toLowerCase().startsWith(lang.slice(0, 2).toLowerCase()));
  const pool = exact.length ? exact : family;
  return pool.find((v) => v.localService && v.default) ?? pool.find((v) => v.localService) ?? pool[0];
}

/** Speaks `text` in `lang` (BCP 47), interrupting anything still being read. False when the
 *  platform has no speech synthesis at all. */
export function speak(text: string, lang: string): boolean {
  if (!synth) return false;
  if (!text.trim()) return true;
  synth.cancel();
  const utterance = new SpeechSynthesisUtterance(text.slice(0, 1000));
  // Without a matching voice the engine still uses its default voice for `lang`.
  const voice = pickVoice(lang);
  if (voice) utterance.voice = voice;
  utterance.lang = voice?.lang ?? lang;
  utterance.volume = state.settings.volume;
  synth.speak(utterance);
  return true;
}

export function stopSpeaking(): void {
  synth?.cancel();
}
