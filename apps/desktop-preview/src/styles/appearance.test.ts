import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

const appearance = readFileSync('src/styles/appearance.module.css', 'utf8');
const conversation = readFileSync('src/components/Conversation.module.css', 'utf8');
const tokens = readFileSync('src/styles/tokens.css', 'utf8');

describe('bounded inherited appearance styles', () => {
  it('preserves default prose and width and never exposes an oversized reading preset or changes code/status tokens', () => {
    expect(conversation).toContain('font-size: var(--prose-size, 15px); line-height: var(--prose-spacing, 1.7);');
    expect(tokens).toContain('--reading-width: 780px;');
    const widths = [...appearance.matchAll(/--reading-width:\s*(\d+)px/g)].map((match) => Number(match[1]));
    expect(widths).toEqual([620, 700]); expect(widths.every((width) => width <= 780)).toBe(true);
    expect(appearance).not.toMatch(/--(?:mono|code|status-[\w-]+):|@font-face|url\(/);
    expect(appearance).toContain(".scope[data-ui-font='system'] { font-family: system-ui, sans-serif; }");
  });
  it('keeps palette overrides below OS contrast/transparency and the unsupported-blur fallback', () => {
    for (const condition of ['@supports not', '@media (prefers-reduced-transparency: reduce)', '@media (prefers-contrast: more)']) {
      const body = appearance.slice(appearance.indexOf(condition)).split('}')[0];
      expect(body).toContain('.scope[data-material]'); expect(body).toContain('--glass-filter: none;'); expect(body).toContain('--material-drift-display: none;');
      expect(body).toContain('--glass-fill: var(--appearance-opaque);');
    }
    expect(appearance.slice(appearance.indexOf('@media (prefers-contrast: more)'))).toContain('--quiet: #c7c7c7;');
  });
  it('reduces app-wide animation, transition and scrolling, including pseudo-elements, while preserving default working rotation', () => {
    expect(appearance).toContain(".scope[data-motion='reduced'] *::before"); expect(appearance).toContain(".scope[data-motion='reduced'] *::after");
    expect(appearance).toContain('animation: none !important; transition: none !important; scroll-behavior: auto !important;');
    expect(appearance).toContain('@media (prefers-reduced-motion: reduce)');
    const status = readFileSync('src/components/ChatStatus.module.css', 'utf8');
    expect(status).toMatch(/2\.4s/); expect(status.match(/animation:/g)).toHaveLength(1);
  });
});
