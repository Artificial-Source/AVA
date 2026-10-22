import { readFileSync } from 'node:fs';
import { StrictMode, type SyntheticEvent } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ToolView } from '../contracts';
import { toolImagePreview } from '../toolImages';
import { tools } from '../../catalog/examples';
import { createInitialState } from '../../state/fixtures';
import ToolActivityDetails from './ToolActivityDetails';
import { ToolImagePreview } from './ToolImagePreview';
import styles from './ToolActivityDetails.module.css';

const previewId = 'search-results-example';
const descriptor = toolImagePreview(previewId)!;
const record: ToolView = { id: 'inspect-image', action: 'Inspect image', detail: 'Compare the pasted results page with the restored filters.', state: 'failed', resource: { kind: 'image', path: '/tmp/pi-clipboard-12345678.png', previewId } };
function loaded(node: HTMLImageElement) {
  Object.defineProperties(node, { naturalWidth: { configurable: true, value: 640 }, naturalHeight: { configurable: true, value: 400 } });
  fireEvent.load(node);
}
function callbacks(node: HTMLImageElement) {
  const key = Object.keys(node).find((key) => key.startsWith('__reactProps'))!;
  return (node as unknown as Record<string, { onLoad: (event: SyntheticEvent<HTMLImageElement>) => void; onError: (event: SyntheticEvent<HTMLImageElement>) => void }>)[key];
}

describe('bounded input image detail', () => {
  it('opts only the normal failed fixture into pixels without changing its facts or catalog/child data', () => {
    const state = createInitialState(); const turn = state.chats[0].messages[1];
    expect(turn.role === 'assistant' && turn.activity?.find((entry) => entry.id === 'inspect-image')).toEqual(record);
    expect(JSON.stringify(state).match(/"previewId"/g)).toHaveLength(1);
    expect(JSON.stringify(state.childAgents)).not.toContain('previewId'); expect(JSON.stringify(tools)).not.toContain('previewId');
  });
  it('leads a failed image row with the failure, then a small input-image thumbnail that enlarges in place', () => {
    const view = render(<StrictMode><ToolActivityDetails record={record} /></StrictMode>);
    const failure = screen.getByText('AVA didn’t report why this failed.');
    const image = view.container.querySelector('img')!;
    expect(failure.compareDocumentPosition(image) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(image).toHaveAttribute('src', descriptor.src); expect(image).toHaveAttribute('alt', descriptor.alt);
    expect(image).toHaveAttribute('width', '640'); expect(image).toHaveAttribute('height', '400'); expect(image).toHaveAttribute('loading', 'lazy'); expect(image).toHaveAttribute('decoding', 'async');
    expect(image).not.toHaveAttribute('srcset'); expect(image).toHaveClass(styles.imageLoading); expect(screen.getByText('Loading image…')).toBeInTheDocument();
    loaded(image); expect(image).toHaveAccessibleName(descriptor.alt); expect(image).not.toHaveClass(styles.imageLoading);
    expect(screen.queryByText('Loading image…')).not.toBeInTheDocument(); expect(screen.getByText('Input image')).toBeVisible();
    expect(screen.getByText(record.detail)).toBeVisible(); expect(screen.getByText(record.resource!.path)).toBeVisible(); expect(screen.queryByText(/^No output/)).not.toBeInTheDocument();
    const enlarge = screen.getByRole('button', { name: /^Enlarge image/ }); const figure = view.container.querySelector('figure')!;
    expect(enlarge).toHaveAttribute('aria-pressed', 'false'); expect(figure).toHaveAttribute('data-enlarged', 'false');
    fireEvent.click(enlarge); expect(enlarge).toHaveAttribute('aria-pressed', 'true'); expect(figure).toHaveAttribute('data-enlarged', 'true'); expect(view.container.querySelector('img')).toBe(image);
    fireEvent.click(enlarge); expect(enlarge).toHaveAttribute('aria-pressed', 'false');
    expect(view.container.querySelectorAll('button')).toHaveLength(1); expect(view.container.querySelector('a, input, iframe, video, canvas')).toBeNull();
  });
  it('names a reported failure cause instead of the generic line', () => {
    render(<ToolActivityDetails record={{ ...record, error: 'The clipboard image was too large to read.' }} />);
    expect(screen.getByText('The clipboard image was too large to read.')).toBeVisible(); expect(screen.queryByText('AVA didn’t report why this failed.')).not.toBeInTheDocument();
  });
  it.each([undefined, null, 'unknown', '__proto__', 'constructor', 'toString', 'https://example.invalid/image.png', '/tool-previews/search-results.png', 'file:///tmp/image.png', 'blob:example', 'data:image/png;base64,abc', '<svg/>', { src: descriptor.src }])('renders unavailable without img or host fetch for absent/invalid IDs: %j', (previewId) => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const view = render(<ToolImagePreview previewId={previewId} />);
    expect(screen.getByText('No preview available for this image.')).toBeVisible(); expect(view.container.querySelector('img, a, button, input')).toBeNull();
    spies.forEach((spy) => expect(spy).not.toHaveBeenCalled());
  });
  it('never guesses from image paths, action, output or an invalid file preview field', () => {
    const view = render(<ToolActivityDetails record={{ ...record, resource: { kind: 'image', path: descriptor.src }, output: { text: '<img src="/tool-previews/search-results.png">', truncated: false } }} />);
    expect(view.container.querySelector('img')).toBeNull(); expect(screen.getByText('No preview available for this image.')).toBeVisible();
    view.rerender(<ToolActivityDetails record={{ ...record, resource: { kind: 'file', path: descriptor.src, previewId } } as unknown as ToolView} />);
    expect(view.container.querySelector('img')).toBeNull(); expect(screen.queryByText(/No preview available/)).not.toBeInTheDocument();
  });
  it.each(['error', 'empty-load'] as const)('collapses a broken/undecodable image to one line on %s, keeping path, output and diff', (failure) => {
    const view = render(<ToolActivityDetails record={{ ...record, output: { text: 'Tool output', truncated: true }, changes: [{ id: 'c', filename: 'note.txt', operation: 'modified', diff: { state: 'text', text: '-old\n+new', truncated: false } }] }} />);
    const image = view.container.querySelector('img')!; const old = callbacks(image);
    if (failure === 'error') fireEvent.error(image); else fireEvent.load(image); // JSDOM has no decoded pixels.
    expect(view.container.querySelector('img, figure')).toBeNull(); expect(screen.getByText('Couldn’t load the input image.')).toBeVisible(); expect(screen.queryByText('Input image')).not.toBeInTheDocument();
    act(() => old.onLoad({ currentTarget: image } as SyntheticEvent<HTMLImageElement>));
    expect(view.container.querySelector('img, figure')).toBeNull();
    expect(screen.getByText(record.detail)).toBeVisible(); expect(screen.getByText(record.resource!.path)).toBeVisible(); expect(screen.getByLabelText('Output: Inspect image')).toHaveTextContent('Tool output'); expect(screen.getByLabelText('Diff: note.txt')).toHaveTextContent('-old');
  });
  it('keeps the img and current load ownership across same-ID immutable status/text/output/path updates without moving editor selection', () => {
    const content = (record: ToolView) => <StrictMode><ToolActivityDetails record={record} /><textarea aria-label="Draft" defaultValue="Keep this draft" /></StrictMode>;
    const view = render(content({ ...record, state: 'running' })); const image = view.container.querySelector('img')!; const old = callbacks(image);
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 8); const focus = vi.spyOn(editor, 'focus');
    view.rerender(content({ ...record, resource: { kind: 'image', path: 'different-text-only.png', previewId }, output: { text: 'More tool output', truncated: false } }));
    expect(view.container.querySelector('img')).toBe(image);
    Object.defineProperties(image, { naturalWidth: { value: 640 }, naturalHeight: { value: 400 } });
    act(() => old.onLoad({ currentTarget: image } as SyntheticEvent<HTMLImageElement>)); // Same live image, not a stale tool snapshot.
    expect(image).not.toHaveClass(styles.imageLoading); expect(editor).toHaveFocus(); expect(editor).toHaveValue('Keep this draft'); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(8); expect(focus).not.toHaveBeenCalled();
    expect(image).toHaveAttribute('src', descriptor.src);
  });
  it('ignores detached, other-img and old-source callbacks, and resets only when image ownership departs', () => {
    const view = render(<ToolImagePreview previewId={previewId} />); const first = view.container.querySelector('img')!; const old = callbacks(first);
    const other = document.createElement('img'); other.src = descriptor.src;
    act(() => old.onError({ currentTarget: other } as SyntheticEvent<HTMLImageElement>)); expect(first).toBeInTheDocument();
    first.setAttribute('src', '/tool-previews/old-source.png');
    act(() => { old.onLoad({ currentTarget: first } as SyntheticEvent<HTMLImageElement>); old.onError({ currentTarget: first } as SyntheticEvent<HTMLImageElement>); });
    expect(first).toBeInTheDocument(); expect(first).toHaveClass(styles.imageLoading);
    first.setAttribute('src', descriptor.src);
    view.rerender(<ToolImagePreview previewId={null} />); expect(first).not.toBeInTheDocument();
    view.rerender(<ToolImagePreview previewId={previewId} />); const second = view.container.querySelector('img')!; expect(second).not.toBe(first);
    act(() => { old.onError({ currentTarget: first } as SyntheticEvent<HTMLImageElement>); old.onLoad({ currentTarget: second } as SyntheticEvent<HTMLImageElement>); });
    expect(second).toBeInTheDocument(); loaded(second); expect(second).not.toHaveClass(styles.imageLoading);
    const departed = callbacks(second); view.unmount();
    act(() => { departed.onError({ currentTarget: second } as SyntheticEvent<HTMLImageElement>); departed.onLoad({ currentTarget: second } as SyntheticEvent<HTMLImageElement>); });
    expect(view.container).toBeEmptyDOMElement();
  });
  it('reserves a bounded thumbnail frame through loading without timers or auto-scroll machinery', () => {
    const css = readFileSync('src/chat/widgets/ToolActivityDetails.module.css', 'utf8');
    expect(css).toContain('.imagePreview { width: 160px; max-width: 100%;'); expect(css).toContain('.imagePreview[data-enlarged=\'true\'] { width: 640px; }');
    expect(css).toContain('.imageFrame { position: relative; aspect-ratio: 8 / 5; max-height: 400px; overflow: hidden; }');
    expect(css).toContain('max-width: 100%; max-height: 400px; object-fit: contain;');
    vi.useFakeTimers();
    try { const view = render(<ToolImagePreview previewId={previewId} />); loaded(view.container.querySelector('img')!); fireEvent.error(view.container.querySelector('img')!); expect(vi.getTimerCount()).toBe(0); }
    finally { vi.useRealTimers(); }
  });
});
