import { StrictMode } from 'react';
import { act, render, screen, waitFor } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import type { ThemedToken } from 'shiki/types';
import { CodeBlock } from './CodeBlock';

const engine = vi.hoisted(() => ({ highlight: vi.fn<(text: string) => Promise<ThemedToken[][]>>() }));
vi.mock('../highlight', () => ({ highlightTypeScript: engine.highlight }));
it('does not load/highlight changing partial prefixes; highlights the terminal snapshot and ignores late results after replacement/unmount', async () => {
  let resolve!: (lines: ThemedToken[][]) => void; engine.highlight.mockImplementation(() => new Promise((done) => { resolve = done; }));
  const view = render(<StrictMode><CodeBlock filename="stream.ts" code="const" partial /></StrictMode>);
  const output = screen.getByLabelText('stream.ts, illustrative TypeScript');
  for (let i = 1; i <= 1000; i++) view.rerender(<StrictMode><CodeBlock filename="stream.ts" code={`const text = "${'x'.repeat(i)}`} partial /></StrictMode>);
  expect(engine.highlight).not.toHaveBeenCalled(); expect(screen.getByLabelText('stream.ts, illustrative TypeScript')).toBe(output);
  view.rerender(<StrictMode><CodeBlock filename="stream.ts" code={'const text = "final";'} /></StrictMode>);
  await waitFor(() => expect(engine.highlight).toHaveBeenCalledExactlyOnceWith('const text = "final";'));
  view.rerender(<StrictMode><CodeBlock filename="stream.ts" code="next partial" partial /></StrictMode>);
  await act(async () => { resolve([[{ content: 'old highlighted', color: '#ffffff', offset: 0 }]]); }); expect(output.textContent).toBe('next partial');
  view.unmount(); expect(engine.highlight).toHaveBeenCalledTimes(1);
});
