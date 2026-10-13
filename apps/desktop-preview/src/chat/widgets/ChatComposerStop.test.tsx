import { useState } from 'react';
import { flushSync } from 'react-dom';
import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ComposerView } from '../contracts';
import { ChatComposer } from './ChatComposer';

const composer: ComposerView = { title: 'Stop regression', draft: 'Keep this unsent draft.', expanded: false, runState: 'running', contextOptions: [], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }], modeId: 'build' };
function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn() }; }

describe('composer Stop and Send identities', () => {
  it('prevents the Stop default before a synchronous idle update and sends only on a subsequent explicit Send', async () => {
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
    const stop = screen.getByRole('button', { name: 'Request stop' });

    fireEvent(stop, click);
    expect(preventedBeforeStop).toEqual([true]);
    expect(click.defaultPrevented).toBe(true);
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(submitted).not.toHaveBeenCalled();
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onDraftChange).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(composer.draft);
    expect(stop.isConnected).toBe(false);
    expect(stop).toHaveAttribute('type', 'button');
    const send = screen.getByRole('button', { name: 'Send' });
    expect(send).not.toBe(stop);
    expect(send).toHaveAttribute('type', 'submit');
    fireEvent.click(stop);
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(cb.onSend).not.toHaveBeenCalled();

    await user.click(send);
    expect(submitted).toHaveBeenCalledTimes(1);
    expect(submitted.mock.calls[0][0].submitter).toBe(send);
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    expect(cb.onStop).toHaveBeenCalledTimes(1);
    expect(draft).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Send' })).toBe(send);
    expect(send).toBeDisabled();
    await user.click(send);
    expect(cb.onSend).toHaveBeenCalledTimes(1);
  });

  it('does not turn a pending Stop pointer press into Send when the run settles before release', async () => {
    const user = userEvent.setup();
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} />);
    const stop = screen.getByRole('button', { name: 'Request stop' });
    const draft = screen.getByRole('textbox');
    rerender(<ChatComposer view={{ ...composer, expanded: true }} {...cb} />);
    expect(screen.getByRole('button', { name: 'Request stop' })).toBe(stop);

    await user.pointer({ target: stop, keys: '[MouseLeft>]' });
    rerender(<ChatComposer view={{ ...composer, runState: 'idle' }} {...cb} />);
    const send = screen.getByRole('button', { name: 'Send' });
    // jsdom has no hit testing; explicitly release over the new control.
    await user.pointer({ target: send, keys: '[/MouseLeft]' });
    expect(cb.onSend).not.toHaveBeenCalled();
    fireEvent.click(stop);
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onStop).not.toHaveBeenCalled();
    expect(cb.onDraftChange).not.toHaveBeenCalled();
    expect(stop.isConnected).toBe(false);
    expect(stop).toHaveAttribute('type', 'button');
    expect(send).not.toBe(stop);
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(composer.draft);

    await user.click(send);
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    expect(cb.onStop).not.toHaveBeenCalled();
  });
});
