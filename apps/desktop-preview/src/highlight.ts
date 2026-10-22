import { createHighlighterCore } from 'shiki/core';
import { createJavaScriptRegexEngine } from 'shiki/engine/javascript';
import typescript from 'shiki/langs/typescript.mjs';
import type { ThemedToken, ThemeRegistration } from 'shiki/types';

// First-party neutral theme: syntax is distinguished by luminance, never hue.
const monochrome = {
  name: 'ava-monochrome',
  type: 'dark',
  fg: '#d4d4d4',
  bg: '#111111',
  settings: [
    { scope: ['comment'], settings: { foreground: '#a1a1a1' } },
    { scope: ['keyword', 'storage'], settings: { foreground: '#eeeeee' } },
    { scope: ['string'], settings: { foreground: '#b8b8b8' } },
    { scope: ['constant', 'entity.name'], settings: { foreground: '#dedede' } },
    { scope: ['punctuation'], settings: { foreground: '#a9a9a9' } },
  ],
} satisfies ThemeRegistration;

// One grammar, one theme, one cached JS-regex engine. No Wasm, CDN, or HTML injection.
let highlighter: ReturnType<typeof createHighlighterCore> | undefined;
const fixtureTokens = new Map<string, Promise<ThemedToken[][]>>();

export function highlightTypeScript(code: string): Promise<ThemedToken[][]> {
  const cached = fixtureTokens.get(code);
  if (cached) return cached;
  highlighter ??= createHighlighterCore({
    langs: [typescript],
    themes: [monochrome],
    engine: createJavaScriptRegexEngine(),
  });
  const result = highlighter.then((instance) =>
    instance.codeToTokens(code, { lang: 'typescript', theme: monochrome.name }).tokens,
  );
  fixtureTokens.set(code, result);
  return result;
}
