import { StrictMode } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it } from 'vitest';
import { createInitialState } from '../state/fixtures';
import type { Chat, Message } from '../state/types';
import { defaultConversation } from '../screens/behaviorPreferences';
import { Conversation } from './Conversation';
import { ToolActivity } from './ToolActivity';

function assistant(id: string, text = `Output for ${id}`): Message {
  return { id, role: 'assistant', markdown: `Response ${id}`, attribution: { modelLabel: 'Aster', thinkingLabel: 'High' }, activity: [{ id: `${id}-tool`, action: `Read ${id}`, detail: `${id}.txt`, state: 'completed', output: { text, truncated: false } }] };
}
function conversation(): Chat {
  return { ...createInitialState().chats[0], messages: [assistant('first'), assistant('second')], changes: [] };
}
function groupButtons() { return screen.getAllByRole('button', { name: /^Tool details/ }); }

describe('conversation-wide tool expansion', () => {
  it('opens every assistant group with one real control, keeps local overrides and collapses everything without mutating supplied records', async () => {
    const user = userEvent.setup(); const chat = conversation();
    const snapshot = JSON.stringify(chat);
    for (const message of chat.messages) { if (message.role === 'assistant') { message.activity?.forEach(Object.freeze); Object.freeze(message.activity); } Object.freeze(message); }
    Object.freeze(chat.messages); Object.freeze(chat);
    render(<StrictMode><Conversation chat={chat} /></StrictMode>);
    const triggers = groupButtons(); expect(triggers).toHaveLength(2);
    triggers.forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
    expect(screen.queryByText('Output for first')).not.toBeInTheDocument();
    const expand = screen.getByRole('button', { name: 'Expand all tools' }); expand.focus(); await user.keyboard('{Enter}');
    expect(await screen.findByText('Output for first')).toBeVisible(); expect(await screen.findByText('Output for second')).toBeVisible();
    expect(expand).toHaveFocus(); expect(expand).toHaveAccessibleName('Collapse all tools');
    triggers.forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'true'));
    expect(screen.getAllByRole('list', { name: 'Supplied tool activity' })).toHaveLength(2);
    expect(screen.getAllByRole('listitem')).toHaveLength(2); // Not summary + output lists.
    await user.click(triggers[0]); expect(triggers[0]).toHaveFocus();
    expect(screen.queryByText('Output for first')).not.toBeInTheDocument(); expect(screen.getByText('Output for second')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); expect(await screen.findByText('Output for first')).toBeVisible();
    const collapse = screen.getByRole('button', { name: 'Collapse all tools' }); collapse.focus(); await user.keyboard(' ');
    expect(collapse).toHaveFocus(); expect(screen.queryByText('Output for first')).not.toBeInTheDocument(); expect(screen.queryByText('Output for second')).not.toBeInTheDocument();
    expect(JSON.stringify(chat)).toBe(snapshot);
    expect(screen.getAllByLabelText('Model: Aster')).toHaveLength(2); expect(screen.getAllByLabelText('Thinking: High')).toHaveLength(2);
  });
  it('retains same-ID rerecords, starts new/reintroduced groups collapsed and prunes removed groups even when a caller reuses its chat object', async () => {
    const user = userEvent.setup(); const chat = conversation(); const view = render(<Conversation chat={chat} />);
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByText('Output for first');
    chat.messages = [assistant('first', 'Latest supplied output'), assistant('second'), assistant('new')];
    view.rerender(<Conversation chat={chat} />);
    expect(screen.queryByText('Output for first')).not.toBeInTheDocument(); expect(screen.getByText('Latest supplied output')).toBeVisible();
    expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'true', 'false']);
    const departed = groupButtons()[1];
    chat.messages = [assistant('first')]; view.rerender(<Conversation chat={chat} />); fireEvent.click(departed);
    chat.messages = [assistant('first'), assistant('second')]; view.rerender(<Conversation chat={chat} />);
    expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'false']);
    chat.messages = []; view.rerender(<Conversation chat={chat} />);
    expect(screen.queryByRole('button', { name: /all tools/ })).not.toBeInTheDocument();
    chat.messages = [assistant('first')]; view.rerender(<Conversation chat={chat} />);
    expect(groupButtons()[0]).toHaveAttribute('aria-expanded', 'false');
  });
  it('does not let an empty activity group inherit an earlier expansion on the same message', async () => {
    const user = userEvent.setup(); const chat = conversation(); const view = render(<Conversation chat={chat} />);
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByText('Output for first');
    chat.messages = [{ ...assistant('first'), activity: [] } as Message]; view.rerender(<Conversation chat={chat} />);
    expect(screen.getByRole('button', { name: 'No tool activity' })).toBeDisabled();
    expect(screen.queryByRole('button', { name: /all tools/ })).not.toBeInTheDocument();
    chat.messages = [assistant('first')]; view.rerender(<Conversation chat={chat} />);
    expect(groupButtons()[0]).toHaveAttribute('aria-expanded', 'false');
  });
  it('scopes arbitrary IDs to the current chat and resets on replacement/remount without object-key aliases', async () => {
    const user = userEvent.setup(); const chat = { ...conversation(), messages: [assistant('__proto__'), assistant('constructor')] };
    const view = render(<Conversation chat={chat} />);
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByText('Output for __proto__');
    view.rerender(<Conversation chat={{ ...chat, id: 'another-chat' }} />);
    groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
    view.rerender(<Conversation chat={chat} />);
    groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByText('Output for constructor');
    view.unmount(); render(<Conversation chat={chat} />);
    groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
  });
  it('expands all explicitly supplied inline diffs together, never joins a current Changes inventory, and retains authored notes', async () => {
    const user = userEvent.setup(); const chat = createInitialState().chats[0];
    const original = chat.changes.map((change) => ({ ...change }));
    chat.changes = [{ filename: 'unrelated-file.ts', description: 'Different inspector data', before: 'unrelated-before', after: 'unrelated-after' }];
    render(<Conversation chat={chat} />);
    expect(screen.queryByLabelText(/^Supplied diff:/)).not.toBeInTheDocument();
    expect(screen.getByText('Browser back/forward behavior still needs a manual check.')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Expand all tools' }));
    for (const file of original) {
      const diff = await screen.findByLabelText(`Supplied diff: ${file.filename}`);
      for (const line of file.before.split('\n')) expect(diff.textContent).toContain(`-${line}`);
      for (const line of file.after.split('\n')) expect(diff.textContent).toContain(`+${line}`);
    }
    expect(screen.queryByText('unrelated-file.ts')).not.toBeInTheDocument();
    expect(screen.getAllByRole('article', { name: 'AVA example response' })).toHaveLength(1);
    expect(screen.getAllByRole('listitem')).toHaveLength(3);
    await user.click(screen.getByRole('button', { name: 'Collapse all tools' }));
    expect(screen.queryByLabelText(/^Supplied diff:/)).not.toBeInTheDocument();
    expect(screen.getByRole('list', { name: 'Supplied tool activity' })).toHaveTextContent('Pasted image Failed');
  });
  it('preserves the optional standalone adapter without overriding a controlled parent', async () => {
    const user = userEvent.setup(); const records = [{ id: 'one', action: 'Read', detail: 'Example', state: 'completed' as const, output: { text: 'Adapter output', truncated: false } }];
    const view = render(<ToolActivity records={records} />);
    await user.click(groupButtons()[0]); expect(await screen.findByText('Adapter output')).toBeVisible();
    view.unmount(); render(<ToolActivity records={records} open={false} onOpenChange={() => {}} />);
    await user.click(groupButtons()[0]); expect(screen.queryByText('Adapter output')).not.toBeInTheDocument();
    expect(within(screen.getByRole('region', { name: 'Tool activity' })).getAllByRole('listitem')).toHaveLength(1);
  });
});


describe('conversation preferences and explicit overrides', () => {
  it('changes only unoverridden groups, retains same-ID overrides and prunes before reintroduction', async () => {
    const chat = conversation(); const view = render(<Conversation chat={chat} preferences={defaultConversation} />);
    fireEvent.click(groupButtons()[0]); await screen.findByText('Output for first');
    const expanded = { ...defaultConversation, expandTools: true };
    view.rerender(<Conversation chat={chat} preferences={expanded} />); expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'true']);
    fireEvent.click(groupButtons()[1]); view.rerender(<Conversation chat={chat} preferences={defaultConversation} />); expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'false']);
    chat.messages = [assistant('first', 'Same ID replacement'), assistant('second'), assistant('third')]; view.rerender(<Conversation chat={chat} preferences={expanded} />);
    expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'false', 'true']);
    chat.messages = [assistant('first')]; view.rerender(<Conversation chat={chat} preferences={expanded} />);
    chat.messages = [assistant('first'), assistant('second')]; view.rerender(<Conversation chat={chat} preferences={expanded} />); expect(groupButtons()[1]).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); view.rerender(<Conversation chat={chat} preferences={expanded} />); groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
  });
  it('keeps reading position when requested; only a new visible local send with scrolling enabled moves it', () => {
    let chat = conversation(); const keep = { ...defaultConversation, scrollAfterSend: false }; const view = render(<Conversation chat={chat} preferences={keep} />);
    const transcript = screen.getByRole('region', { name: /^Conversation:/ }); Object.defineProperty(transcript, 'scrollHeight', { value: 900 }); transcript.scrollTop = 70;
    chat = { ...chat, messages: [...chat.messages, { id: 'local-1', role: 'user', origin: 'local', text: 'First' }] }; view.rerender(<Conversation chat={chat} preferences={keep} />); expect(transcript.scrollTop).toBe(70);
    view.rerender(<Conversation chat={chat} preferences={defaultConversation} />); expect(transcript.scrollTop).toBe(70);
    chat = { ...chat, messages: [...chat.messages, { id: 'local-2', role: 'user', origin: 'local', text: 'Second' }] }; view.rerender(<Conversation chat={chat} preferences={defaultConversation} />); expect(transcript.scrollTop).toBe(900);
    transcript.hidden = true; transcript.scrollTop = 70; chat = { ...chat, messages: [...chat.messages, { id: 'local-3', role: 'user', origin: 'local', text: 'Hidden' }] }; view.rerender(<Conversation chat={chat} preferences={defaultConversation} />); expect(transcript.scrollTop).toBe(70);
  });
  it('preserves errors and supplied file changes while hiding completed output, with no duplicate outcomes', async () => {
    const chat = createInitialState().chats[0];
    for (const message of chat.messages) if (message.role === 'assistant') message.activity = message.activity?.map((record) => ({ ...record, output: { text: record.state === 'failed' ? 'Supplied failure output' : 'Supplied completed output', truncated: false } }));
    const before = JSON.stringify(chat);
    render(<Conversation chat={chat} preferences={{ ...defaultConversation, expandTools: true, showCompletedOutput: false }} />);
    expect(await screen.findAllByLabelText(/^Supplied diff:/)).toHaveLength(2);
    expect(screen.getAllByLabelText(/^Output:/)).toHaveLength(1); expect(screen.getByRole('list', { name: 'Supplied tool activity' })).toHaveTextContent('Failed');
    expect(screen.getAllByRole('listitem')).toHaveLength(3); expect(JSON.stringify(chat)).toBe(before); expect(screen.getByLabelText('Thinking: High')).toBeVisible();
  });
});
