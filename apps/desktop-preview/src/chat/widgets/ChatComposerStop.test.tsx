import { useState } from 'react';
import { flushSync } from 'react-dom';
import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ComposerView } from '../contracts';
import { ChatComposer } from './ChatComposer';

const composer: ComposerView = { title: 'Stop regression', draft: 'Keep this unsent draft.', expanded: false, runState: 'running', contextOptions: [], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }], modeId: 'build' };
function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn() }; }

describe('composer Send/Stop action', () => {
  it('prevents the Stop default before a synchronous idle update, ignores the double-click that follows, and sends only on a later explicit Send', async () => {
    const user = userEvent.setup();
    const cb = callbacks();
    const click = new MouseEvent('click', { bubbles: true, cancelable: true });
    const preventedBeforeStop: boolean[] = [];
    function Host() {
      const [view, setView] = useState(composer);
      return <ChatComposer view={view} {...cb}
        onStop={() => {
          preventedBeforeStop.push(click.defaultPrevented);
          cb.onStop();
          // Commit idle before the native click default can inspect the button.
          flushSync(() => setView((current) => ({ ...current, runState: 'idle' })));
        }}
        onSend={() => {
          cb.onSend();
          setView((current) => ({ ...current, draft: '' }));
        }}
      />;
    }
    render(<Host />);
    const form = screen.getByRole('form', { name: 'Message composer' });
    const submitted = vi.fn();
    form.addEventListener('submit', submitted);
    const draft = screen.getByRole('textbox');
    const action = screen.getByRole('button', { name: 'Request stop' });
    expect(action).toHaveAttribute('type', 'button');

    fireEvent(action, click);
    expect(preventedBeforeStop).toEqual([true]);
    expect(click.defaultPrevented).toBe(true);
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(submitted).not.toHaveBeenCalled();
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onDraftChange).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(composer.draft);
    // The same element becomes Send, so focus and position never jump.
    expect(screen.getByRole('button', { name: 'Send' })).toBe(action);
    expect(action).toHaveAttribute('type', 'submit');
    // The second click of a double-click on Stop never sends the draft.
    fireEvent.click(action, { detail: 2 });
    expect(submitted).not.toHaveBeenCalled();
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onStop).toHaveBeenCalledTimes(1);

    await user.click(action);
    expect(submitted).toHaveBeenCalledTimes(1);
    expect(submitted.mock.calls[0][0].submitter).toBe(action);
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(draft).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Send' })).toBe(action);
    // Empty draft: unavailable but still focusable, so focus stays on the control.
    expect(action).toHaveAttribute('aria-disabled', 'true');
    expect(action).not.toBeDisabled();
    expect(action).toHaveFocus();
    await user.click(action);
    expect(submitted).toHaveBeenCalledTimes(1);
    expect(cb.onSend).toHaveBeenCalledTimes(1);
  });

  it('does not turn a pending Stop pointer press into Send when the run settles before release', async () => {
    const user = userEvent.setup();
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} />);
    const action = screen.getByRole('button', { name: 'Request stop' });
    const draft = screen.getByRole('textbox');
    rerender(<ChatComposer view={{ ...composer, expanded: true }} {...cb} />);
    expect(screen.getByRole('button', { name: 'Request stop' })).toBe(action);

    await user.pointer({ target: action, keys: '[MouseLeft>]' });
    rerender(<ChatComposer view={{ ...composer, runState: 'idle' }} {...cb} />);
    expect(screen.getByRole('button', { name: 'Send' })).toBe(action);
    await user.pointer({ target: action, keys: '[/MouseLeft]' });
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onStop).not.toHaveBeenCalled();
    expect(cb.onDraftChange).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(composer.draft);

    // A fresh press that starts as Send completes as Send.
    await user.pointer({ target: action, keys: '[MouseLeft>]' });
    await user.pointer({ target: action, keys: '[/MouseLeft]' });
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    expect(cb.onStop).not.toHaveBeenCalled();
  });

  it('keeps keyboard focus on the one action button through Stop and Send', async () => {
    const user = userEvent.setup();
    const cb = callbacks();
    function Host() {
      const [view, setView] = useState({ ...composer, draft: '' });
      return <ChatComposer view={view} {...cb}
        onDraftChange={(next) => setView((current) => ({ ...current, draft: next }))}
        onStop={() => { cb.onStop(); setView((current) => ({ ...current, runState: 'idle' })); }}
        onSend={() => { cb.onSend(); setView((current) => ({ ...current, draft: '', runState: 'running' })); }}
      />;
    }
    render(<Host />);
    const action = screen.getByRole('button', { name: 'Request stop' });
    action.focus();
    await user.keyboard('{Enter}');
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(action).toHaveFocus();
    expect(action).toHaveAccessibleName('Send');
    expect(action).toHaveAttribute('aria-disabled', 'true');
    await user.keyboard('{Enter}');
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(action).toHaveFocus();

    await user.type(screen.getByRole('textbox'), 'Next step');
    action.focus();
    await user.keyboard('{Enter}');
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    expect(action).toHaveFocus();
    expect(action).toHaveAccessibleName('Request stop');
    expect(document.activeElement).not.toBe(document.body);
  });
});
