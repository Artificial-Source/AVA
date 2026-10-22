import { readFileSync } from 'node:fs';
import { render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { highlightTypeScript } from '../highlight';
import { CodeBlock } from './CodeBlock';
import { SafeMarkdown } from './SafeMarkdown';

const example = 'const label = "<b>example</b>";\nconst tags = ["one", "two"];';
// Vitest stubs CSS imports, so read the source as the other style tests do.
const codeStyles = readFileSync('src/components/CodeBlock.module.css', 'utf8');

describe('fine-grained TypeScript highlighting', () => {
  it('reuses the same cached token promise for unchanged fixture code', async () => {
    const first = highlightTypeScript(example);
    const second = highlightTypeScript(example);
    expect(first).toBe(second);
    const tokens = await first;
    expect(tokens.map((line) => line.map((token) => token.content).join('')).join('\n')).toBe(example);
    expect(tokens.flat().some((token) => Boolean(token.color))).toBe(true);
  });

  it('resolves syntax roles to scoped neutral variables with a higher-contrast value for each', async () => {
    const tokens = await highlightTypeScript(`// A neutral comment\n${example}\nconst enabled: boolean = true;`);
    const colors = new Set(tokens.flat().map((token) => token.color));
    expect(colors.size).toBeGreaterThan(1);
    const declared = [...codeStyles.matchAll(/(--syntax-[a-z]+):\s*(#[0-9a-f]{6})/gi)];
    const names = [...new Set(declared.map(([, name]) => name))];
    for (const color of colors) expect(names).toContain(/^var\((--syntax-[a-z]+)\)$/.exec(color ?? '')?.[1]);
    // Default, explicit higher contrast, and prefers-contrast: more.
    for (const name of names) expect(declared.filter(([, declaredName]) => declaredName === name)).toHaveLength(3);
    for (const [, , value] of declared) {
      expect(value.slice(1, 3)).toBe(value.slice(3, 5));
      expect(value.slice(3, 5)).toBe(value.slice(5, 7));
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

describe('fenced Markdown code', () => {
  it('keeps one framed block from an arriving fence through completion, while inline code stays a chip', async () => {
    const opening = 'Call `restore` first:\n\n```ts\nconst query = params.get("q");';
    const view = render(<SafeMarkdown partial>{'Call `restore` first:\n\n```t'}</SafeMarkdown>);
    expect(view.container.textContent).not.toContain('`');
    expect(screen.queryByLabelText('TypeScript code block')).not.toBeInTheDocument();
    view.rerender(<SafeMarkdown partial>{opening}</SafeMarkdown>);
    const block = screen.getByLabelText('TypeScript code block');
    expect(block.textContent).toBe('const query = params.get("q");');
    expect(block.closest('figure')).toHaveTextContent('TypeScript');
    expect(screen.getByText('restore', { selector: 'code' }).closest('pre')).toBeNull();
    view.rerender(<SafeMarkdown partial>{`${opening}\n\`\``}</SafeMarkdown>);
    expect(view.container.textContent).not.toContain('`');
    expect(block.querySelector('span')).toBeNull();
    view.rerender(<SafeMarkdown>{`${opening}\n\`\`\``}</SafeMarkdown>);
    expect(screen.getByLabelText('TypeScript code block')).toBe(block);
    await waitFor(() => expect(block.querySelector('span')).not.toBeNull());
    expect(block.textContent).toBe('const query = params.get("q");');
  });

  it('closes unfinished inline syntax while streaming instead of flashing markers', () => {
    const view = render(<SafeMarkdown partial>{'Keep the **saved filters'}</SafeMarkdown>);
    expect(screen.getByText('saved filters', { selector: 'strong' })).toBeVisible();
    expect(view.container.textContent).toBe('Keep the saved filters');
    view.rerender(<SafeMarkdown partial>{'Keep the **saved filters** and `restore'}</SafeMarkdown>);
    expect(screen.getByText('restore', { selector: 'code' })).toBeVisible();
    view.rerender(<SafeMarkdown partial>{'Keep the **saved filters** and read [the guide](https://exa'}</SafeMarkdown>);
    expect(view.container.textContent).toBe('Keep the saved filters and read the guide');
    expect(view.container.querySelector('a')).toBeNull();
  });
});
