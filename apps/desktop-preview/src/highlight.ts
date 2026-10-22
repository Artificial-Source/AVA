import { createHighlighterCore } from 'shiki/core';
import { createJavaScriptRegexEngine } from 'shiki/engine/javascript';
import typescript from 'shiki/langs/typescript.mjs';
import type { HighlighterCore, ThemedToken, ThemeRegistration } from 'shiki/types';

// First-party neutral theme: syntax is distinguished by luminance, never hue. Token colors are
// `--syntax-*` references; CodeBlock.module.css owns the values and their higher-contrast variants.
const monochrome = {
  name: 'ava-monochrome',
  type: 'dark',
  fg: 'var(--syntax-foreground)',
  bg: 'var(--code)',
  settings: [
    { scope: ['comment'], settings: { foreground: 'var(--syntax-comment)' } },
    { scope: ['keyword', 'storage'], settings: { foreground: 'var(--syntax-keyword)' } },
    { scope: ['string'], settings: { foreground: 'var(--syntax-string)' } },
    { scope: ['constant', 'entity.name'], settings: { foreground: 'var(--syntax-constant)' } },
    { scope: ['punctuation'], settings: { foreground: 'var(--syntax-punctuation)' } },
  ],
} satisfies ThemeRegistration;

// Grammar compilation and tokenizing are long main-thread tasks: run each when the page is idle
// (bounded by a deadline), or after the current task where idle callbacks are unsupported.
function whenIdle<T>(work: () => T | PromiseLike<T>): Promise<T> {
  const { promise, resolve, reject } = Promise.withResolvers<T>();
  const run = () => { try { resolve(work()); } catch (error) { reject(error); } };
  if (typeof requestIdleCallback === 'function') requestIdleCallback(run, { timeout: 1000 });
  else {
    const channel = new MessageChannel();
    channel.port1.onmessage = () => { channel.port1.close(); run(); };
    channel.port2.postMessage(null);
  }
  return promise;
}

// One grammar, one theme, one cached JS-regex engine. No Wasm, CDN, or HTML injection.
let highlighter: Promise<HighlighterCore> | undefined;
// Bounded: response code joins fixture code here, and a long session must not grow without limit.
const tokenCache = new Map<string, Promise<ThemedToken[][]>>();
const tokenCacheLimit = 64;

export function highlightTypeScript(code: string): Promise<ThemedToken[][]> {
  const cached = tokenCache.get(code);
  if (cached) return cached;
  highlighter ??= whenIdle(() => createHighlighterCore({
    langs: [typescript],
    themes: [monochrome],
    engine: createJavaScriptRegexEngine(),
  }));
  const result = highlighter.then((instance) => whenIdle(() =>
    instance.codeToTokens(code, { lang: 'typescript', theme: monochrome.name }).tokens,
  ));
  tokenCache.set(code, result);
  for (const key of tokenCache.keys()) {
    if (tokenCache.size <= tokenCacheLimit) break;
    tokenCache.delete(key);
  }
  return result;
}
