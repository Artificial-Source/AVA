import { StrictMode } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { ToolView, TurnState } from '../chat/contracts';
import { toolImagePreview } from '../chat/toolImages';
import type { ToolDisplay } from '../chat/toolDisplay';
import { defaultConversation } from '../screens/behaviorPreferences';
import * as fixtures from '../state/fixtures';
import * as reducer from '../state/reducer';
import { Conversation } from './Conversation';
import { AppShell } from './AppShell';

const image = toolImagePreview('search-results-example')!;
const record: ToolView = { id: 'same-tool', action: 'Inspect image', detail: 'Supplied detail', state: 'running', resource: { kind: 'image', path: '/tmp/text-only.png', previewId: 'search-results-example' } };
function content(record: ToolView, display: ToolDisplay = 'auto', lifecycle: TurnState = 'streaming') {
  return <StrictMode><Conversation chat={{ ...fixtures.createInitialState().chats[0], messages: [{ id: 'same-turn', role: 'assistant', markdown: 'Supplied text', lifecycle, activity: [record] }] }} preferences={{ ...defaultConversation, toolDisplay: display }} /><textarea aria-label="Draft" defaultValue="Keep draft selection" /></StrictMode>;
}
function load(node: HTMLElement) {
  Object.defineProperties(node, { naturalWidth: { configurable: true, value: 640 }, naturalHeight: { configurable: true, value: 400 } }); fireEvent.load(node);
}
function row() { return screen.getByRole('button', { name: /^Inspect image / }); }

describe('image disclosure and passive R19 reading policy', () => {
  it('opens the actual normal failed Inspect image without touching draft, facts, model, history or reducer authority', async () => {
    const initial = vi.spyOn(fixtures, 'createInitialState'); const reduce = vi.spyOn(reducer, 'previewReducer');
    const view = render(<StrictMode><AppShell /></StrictMode>); const state = initial.mock.results.at(-1)!.value; const before = JSON.stringify(state);
    expect(view.container.querySelector('img')).toBeNull(); const trigger = row(); expect(trigger).toHaveAttribute('aria-expanded', 'false');
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; const draft = editor.value; editor.focus(); editor.setSelectionRange(3, 9); const focus = vi.spyOn(editor, 'focus');
    const model = screen.getByRole('button', { name: 'Model for this chat' }).textContent;
    fireEvent.click(trigger); const preview = await screen.findByAltText(image.alt); load(preview);
    expect(preview).toHaveAttribute('src', image.src); expect(trigger).toHaveTextContent('Failed'); expect(screen.getByText('AVA didn’t report why this failed.')).toBeVisible(); expect(screen.getByText('/tmp/pi-clipboard-12345678.png')).toBeVisible();
    fireEvent.click(trigger); expect(preview).not.toBeInTheDocument(); fireEvent.click(trigger); expect(await screen.findByAltText(image.alt)).not.toBe(preview);
    const display = screen.getByRole('combobox', { name: 'Tool details' });
    fireEvent.change(display, { target: { value: 'expanded' } }); const expanded = await screen.findByAltText(image.alt);
    fireEvent.change(display, { target: { value: 'auto' } }); expect(screen.getByAltText(image.alt)).toBe(expanded);
    fireEvent.change(display, { target: { value: 'compact' } }); expect(view.container.querySelector('img')).toBeNull();
    expect(JSON.stringify(state)).toBe(before); expect(reduce).not.toHaveBeenCalled(); expect(trigger).toHaveTextContent('Failed');
    expect(editor).toHaveFocus(); expect(editor).toHaveValue(draft); expect(editor.selectionStart).toBe(3); expect(editor.selectionEnd).toBe(9); expect(focus).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(model!); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
  });
  it('retains the same img through immutable tool/output snapshots and unmounts on row/group/all/Compact intent', async () => {
    const view = render(content(record, 'expanded')); const preview = await screen.findByAltText(image.alt); load(preview); const trigger = row();
    const next: ToolView = { ...record, detail: 'Updated text', state: 'failed', resource: { ...record.resource! }, output: { text: 'New output', truncated: false } };
    view.rerender(content(next, 'expanded')); expect(screen.getByAltText(image.alt)).toBe(preview); expect(row()).toBe(trigger); expect(trigger).toHaveTextContent('Failed');
    view.rerender(content(next)); expect(screen.getByAltText(image.alt)).toBe(preview);
    fireEvent.click(trigger); expect(view.container.querySelector('img')).toBeNull(); fireEvent.click(trigger); expect(await screen.findByAltText(image.alt)).not.toBe(preview);
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(view.container.querySelector('img')).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByAltText(image.alt);
    fireEvent.click(screen.getByRole('button', { name: /^1 tool\b/ })); expect(view.container.querySelector('img')).toBeNull();
    view.rerender(content(next, 'expanded')); await screen.findByAltText(image.alt);
    view.rerender(content(next, 'compact')); expect(view.container.querySelector('img')).toBeNull();
  });
  it.each(['load', 'error'] as const)('never pins Auto reading for passive image %s delivery', async (event) => {
    const view = render(content(record)); const preview = await screen.findByAltText(image.alt);
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 6);
    if (event === 'load') load(preview); else fireEvent.error(preview);
    view.rerender(content({ ...record, state: 'completed' }, 'auto', 'committed'));
    expect(row()).toHaveAttribute('aria-expanded', 'false'); expect(view.container.querySelector('img')).toBeNull(); expect(screen.queryByText(/Couldn’t load the input image/)).not.toBeInTheDocument();
    expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(6);
  });
  it.each(['pointer', 'wheel'] as const)('retains genuine %s inspection through terminal delivery via the unchanged wrapper', async (inspection) => {
    const view = render(content(record)); const preview = await screen.findByAltText(image.alt); load(preview);
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 6);
    if (inspection === 'pointer') { fireEvent.pointerDown(preview); fireEvent.pointerUp(window); } else fireEvent.wheel(preview, { deltaY: -100 });
    view.rerender(content({ ...record, state: 'completed' }, 'auto', 'committed'));
    expect(row()).toHaveAttribute('aria-expanded', 'true'); expect(screen.getByAltText(image.alt)).toBe(preview); expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(6);
    fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); expect(screen.getByAltText(image.alt)).toBe(preview);
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(preview).not.toBeInTheDocument();
  });
});
