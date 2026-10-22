import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { clearProviderConnections, resetProviderState, setProviderState } from '../screens/providerState';
import { createInitialState } from '../state/fixtures';
import type { Chat } from '../state/types';
import { Composer } from './Composer';

// Provider connections last for the whole visit; each case leaves the visit's start behind it.
afterEach(resetProviderState);

const send = { type: 'sendLocalMessage' };
function renderComposer(index: number, patch: Partial<Chat> = {}) {
  const chat = { ...createInitialState().chats[index], ...patch };
  const dispatch = vi.fn(); const onOpenModel = vi.fn();
  render(<Composer chat={chat} dispatch={dispatch} contextOpen={false} onOpenContext={vi.fn()} onOpenModel={onOpenModel} />);
  return { chat, dispatch, onOpenModel, sendButton: () => screen.getByRole('button', { name: 'Send' }), model: () => screen.getByRole('button', { name: 'Model for this chat' }) };
}
const composer = () => within(screen.getByRole('form', { name: 'Message composer' }));

describe('Send follows the connection of the chat model’s provider', () => {
  it('leaves the seeded chats as they were: connected, sendable, no status on the model', async () => {
    const user = userEvent.setup();
    const { chat, dispatch, sendButton, model } = renderComposer(0);
    expect(chat.model).toBe('aster');
    expect(sendButton()).not.toHaveAttribute('aria-disabled'); expect(sendButton()).toHaveAttribute('title', expect.stringMatching(/^Send · /));
    expect(model()).toHaveTextContent(/^Aster$/); expect(model()).not.toHaveAttribute('aria-describedby');
    expect(composer().queryByRole('status')).not.toBeInTheDocument(); expect(composer().queryByRole('button', { name: 'Choose a model' })).not.toBeInTheDocument();
    await user.click(sendButton());
    expect(dispatch).toHaveBeenCalledExactlyOnceWith(send);
  });

  it('blocks Send with its reason while Aster Lab is disconnected, keeps the draft, and recovers on connect', async () => {
    const user = userEvent.setup();
    const { chat, dispatch, onOpenModel, sendButton, model } = renderComposer(0);
    act(() => setProviderState('aster-lab', { state: 'available', source: 'none' }));
    expect(sendButton()).toBeEnabled(); expect(sendButton()).toHaveAttribute('aria-disabled', 'true');
    expect(sendButton()).toHaveAccessibleDescription('Connect Aster Lab to send with Aster'); expect(sendButton()).toHaveAttribute('title', 'Connect Aster Lab to send with Aster');
    expect(model()).toHaveTextContent('AsterNot connected'); expect(model()).toHaveAccessibleDescription('Aster Lab isn’t connected.');
    // Sighted users see why, with one way forward.
    expect(composer().getByRole('status')).toHaveTextContent(/^Aster Lab isn’t connected, so this chat can’t send\.$/);
    await user.click(composer().getByRole('button', { name: 'Choose a model' }));
    expect(onOpenModel).toHaveBeenCalledTimes(1); expect(model()).toHaveFocus();
    await user.click(sendButton()); fireEvent.keyDown(screen.getByRole('textbox'), { key: 'Enter', ctrlKey: true });
    expect(dispatch).not.toHaveBeenCalledWith(send);
    expect(screen.getByRole('textbox')).toHaveValue(chat.draft);
    act(() => setProviderState('aster-lab', { state: 'connected-example', source: 'stored-example' }));
    expect(sendButton()).not.toHaveAttribute('aria-disabled'); expect(sendButton()).not.toHaveAttribute('aria-describedby');
    expect(model()).not.toHaveTextContent('Not connected');
    expect(composer().queryByRole('status')).not.toBeInTheDocument(); expect(composer().queryByRole('button', { name: 'Choose a model' })).not.toBeInTheDocument();
    await user.click(sendButton());
    expect(dispatch).toHaveBeenCalledExactlyOnceWith(send);
  });

  it('checks only the provider behind this chat’s model, and a fresh start blocks every model', () => {
    const { chat, sendButton, model } = renderComposer(3);
    expect(chat.model).toBe('orion');
    act(() => setProviderState('aster-lab', { state: 'available', source: 'none' }));
    expect(sendButton()).not.toHaveAttribute('aria-disabled'); expect(model()).not.toHaveTextContent('Not connected');
    expect(composer().queryByRole('status')).not.toBeInTheDocument();
    act(() => clearProviderConnections());
    expect(sendButton()).toHaveAttribute('aria-disabled', 'true'); expect(sendButton()).toHaveAccessibleDescription('Connect Orion Lab to send with Orion');
    expect(model()).toHaveAccessibleDescription('Orion Lab isn’t connected.');
    expect(composer().getByRole('status')).toHaveTextContent('Orion Lab isn’t connected, so this chat can’t send.');
  });
});

describe('the Context meter follows the chat’s messages', () => {
  it('reads as not used yet in a chat with no messages, never as unknown', () => {
    const { chat } = renderComposer(3, { messages: [] });
    expect(chat.model).toBe('orion');
    const meter = screen.getByRole('button', { name: 'Open Context · No usage yet' });
    expect(meter).toHaveTextContent(/^No usage yet$/); expect(meter).toHaveAttribute('data-level', 'empty');
    expect(meter).not.toHaveTextContent(/unknown/i);
  });

  it('keeps “Context unknown” for a chat with messages and no measurement', () => {
    const { chat } = renderComposer(3);
    expect(chat.messages.length).toBeGreaterThan(0);
    expect(screen.getByRole('button', { name: 'Open Context · Context unknown' })).toHaveAttribute('data-level', 'unknown');
  });
});
