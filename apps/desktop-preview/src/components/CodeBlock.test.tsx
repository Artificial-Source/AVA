import { render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { highlightTypeScript } from '../highlight';
import { CodeBlock } from './CodeBlock';

const example = 'const label = "<b>example</b>";\nconst tags = ["one", "two"];';

describe('fine-grained TypeScript highlighting', () => {
  it('reuses the same cached token promise for unchanged fixture code', async () => {
    const first = highlightTypeScript(example);
    const second = highlightTypeScript(example);
    expect(first).toBe(second);
    const tokens = await first;
    expect(tokens.map((line) => line.map((token) => token.content).join('')).join('\n')).toBe(example);
    expect(tokens.flat().some((token) => Boolean(token.color))).toBe(true);
  });

  it('uses only neutral RGB token colors while retaining syntax distinctions', async () => {
    const tokens = await highlightTypeScript(`// A neutral comment\n${example}\nconst enabled: boolean = true;`);
    const colors = new Set(tokens.flat().map((token) => token.color));
    expect(colors.size).toBeGreaterThan(1);
    for (const color of colors) {
      expect(color).toMatch(/^#[0-9a-f]{6}$/i);
      expect(color!.slice(1, 3)).toBe(color!.slice(3, 5));
      expect(color!.slice(3, 5)).toBe(color!.slice(5, 7));
    }
  });

  it('renders highlighted React spans rather than interpreting code as HTML', async () => {
    render(<CodeBlock filename="example.ts" code={example} />);
    const block = screen.getByLabelText('example.ts, illustrative TypeScript');
    await waitFor(() => expect(block.querySelector('span')).not.toBeNull());
    expect(block.querySelector('b')).toBeNull();
    expect(block.textContent).toBe(example);
  });
});
