import { StrictMode } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
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
function groupButtons() { return screen.getAllByRole('button', { name: /^\d+ tools?\b/ }); }
function toolLists() { return screen.getAllByRole('region', { name: 'Tool activity' }).map((region) => within(region).getByRole('list')); }

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
    expect(toolLists()).toHaveLength(2);
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
    expect(screen.queryByRole('region', { name: 'Tool activity' })).not.toBeInTheDocument();
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
    expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument();
    expect(screen.getByText('Browser back/forward behavior still needs a manual check.')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Expand all tools' }));
    for (const file of original) {
      const diff = await screen.findByLabelText(`Diff: ${file.filename}`);
      for (const line of file.before.split('\n')) expect(diff.textContent).toContain(`-${line}`);
      for (const line of file.after.split('\n')) expect(diff.textContent).toContain(`+${line}`);
    }
    expect(screen.queryByText('unrelated-file.ts')).not.toBeInTheDocument();
    expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(1);
    expect(screen.getAllByRole('listitem')).toHaveLength(3);
    await user.click(screen.getByRole('button', { name: 'Collapse all tools' }));
    expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument();
    expect(toolLists()[0]).toHaveTextContent('Pasted image Failed');
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
  it('reapplies a different preset, retains same-mode same-ID overrides and prunes before reintroduction', async () => {
    const chat = conversation(); const view = render(<Conversation chat={chat} preferences={defaultConversation} />);
    fireEvent.click(groupButtons()[0]); await screen.findByText('Output for first');
    const expanded = { ...defaultConversation, toolDisplay: 'expanded' as const };
    view.rerender(<Conversation chat={chat} preferences={expanded} />); expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['true', 'true']);
    fireEvent.click(groupButtons()[1]); view.rerender(<Conversation chat={chat} preferences={defaultConversation} />); expect(groupButtons().map((button) => button.getAttribute('aria-expanded'))).toEqual(['false', 'false']);
    view.rerender(<Conversation chat={chat} preferences={expanded} />); fireEvent.click(groupButtons()[1]);
    chat.messages = [assistant('first', 'Same ID replacement'), assistant('second'), assistant('third')]; view.rerender(<Conversation chat={chat} preferences={{ ...expanded }} />);
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
    render(<Conversation chat={chat} preferences={{ ...defaultConversation, toolDisplay: 'expanded' as const, showCompletedOutput: false }} />);
    expect(await screen.findAllByLabelText(/^Diff:/)).toHaveLength(2);
    expect(screen.getAllByLabelText(/^Output:/)).toHaveLength(1); expect(toolLists()[0]).toHaveTextContent('Failed');
    expect(screen.getAllByRole('listitem')).toHaveLength(3); expect(JSON.stringify(chat)).toBe(before); expect(screen.getByLabelText('Thinking: High')).toBeVisible();
  });
  it('says Custom while Expand/Collapse overrides differ from the preset, and choosing any preset, even the current one, clears them', async () => {
    const user = userEvent.setup(); const onDisplayChange = vi.fn();
    render(<Conversation chat={conversation()} preferences={defaultConversation} onDisplayChange={onDisplayChange} />);
    const select = screen.getByRole('combobox', { name: 'Tool details' }) as HTMLSelectElement;
    expect(select).toHaveValue('compact'); expect(within(select).queryByRole('option', { name: 'Custom' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Expand all tools' })); await screen.findByText('Output for first');
    expect(select).toHaveValue('custom'); expect(within(select).getByRole('option', { name: 'Custom' })).toBeDisabled();
    groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'true'));
    // Collapsing everything again shows exactly what Compact shows, so the control names it again.
    await user.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(select).toHaveValue('compact');
    await user.click(groupButtons()[1]); expect(select).toHaveValue('custom');
    fireEvent.change(select, { target: { value: 'compact' } });
    expect(onDisplayChange).toHaveBeenCalledExactlyOnceWith('compact', select);
    expect(select).toHaveValue('compact'); groupButtons().forEach((button) => expect(button).toHaveAttribute('aria-expanded', 'false'));
    expect(screen.queryByText('Output for second')).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Expand all tools' })).toBeVisible();
    fireEvent.change(select, { target: { value: 'compact' } }); expect(onDisplayChange).toHaveBeenCalledTimes(1); // Already showing Compact: no-op.
  });
});

describe('trust moments in the transcript', () => {
  it('shows the Attention chat’s blocked step inline, sends one decision for that message, and keeps the stored receipt after a remount', async () => {
    const user = userEvent.setup(); const onPermissionDecision = vi.fn(); const chat = createInitialState().chats.find((entry) => entry.id === 'export-range')!;
    const view = render(<Conversation chat={chat} onPermissionDecision={onPermissionDecision} />);
    const request = screen.getByRole('region', { name: 'Run the export for the selected range?' });
    expect(within(request).getByText('Needs your decision')).toBeVisible(); expect(within(request).getByText('Requested by AVA')).toBeVisible();
    expect(within(request).getByText(/^npm run export/).tagName).toBe('CODE');
    expect(screen.queryByText(/not part of this preview/)).not.toBeInTheDocument();
    expect(request.closest('[data-permission-pending]')).not.toBeNull();
    await user.click(within(request).getByRole('button', { name: 'Allow once' }));
    expect(onPermissionDecision).toHaveBeenCalledExactlyOnceWith('export-response', 'allow');
    expect(within(request).getByRole('status')).toHaveTextContent('You chose Allow once.');
    expect(within(request).getByRole('button', { name: 'Deny' })).toBeDisabled();
    // The host stores the decision; a fresh mount (another chat and back) shows the same receipt.
    const decided = { ...chat, status: 'idle' as const, messages: chat.messages.map((message) => message.id === 'export-response' && message.role === 'assistant' ? { ...message, permissionDecision: 'allow' as const } : message) };
    view.unmount(); render(<Conversation chat={decided} onPermissionDecision={onPermissionDecision} />);
    const stored = screen.getByRole('region', { name: 'Run the export for the selected range?' });
    expect(within(stored).getByRole('status')).toHaveTextContent('You chose Allow once.'); expect(within(stored).getByRole('button', { name: 'Allow once' })).toHaveAttribute('aria-pressed', 'true');
    expect(stored.closest('[data-permission-pending]')).toBeNull(); expect(screen.queryByText('Needs your decision')).not.toBeInTheDocument();
    fireEvent.click(within(stored).getByRole('button', { name: 'Deny' })); expect(onPermissionDecision).toHaveBeenCalledTimes(1);
  });
  it('offers no inline decision without a host to receive it', () => {
    render(<Conversation chat={createInitialState().chats.find((entry) => entry.id === 'export-range')!} />);
    within(screen.getByRole('group', { name: 'Your decision' })).getAllByRole('button').forEach((button) => expect(button).toBeDisabled());
  });
  it('keeps empty chats logo-only and hides tool controls when a chat has no tools', () => {
    const onDisplayChange = vi.fn(); const chat = createInitialState().chats.find((entry) => entry.id === 'route-config')!;
    const view = render(<Conversation chat={{ ...chat, messages: [] }} onDisplayChange={onDisplayChange} />);
    expect(screen.queryByRole('combobox')).not.toBeInTheDocument(); expect(screen.queryByRole('button')).not.toBeInTheDocument();
    view.rerender(<Conversation chat={chat} onDisplayChange={onDisplayChange} />);
    expect(screen.queryByRole('combobox', { name: 'Tool details' })).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: /all tools/ })).not.toBeInTheDocument();
    view.rerender(<Conversation chat={conversation()} onDisplayChange={onDisplayChange} />);
    expect(screen.getByRole('combobox', { name: 'Tool details' })).toBeVisible(); expect(screen.getAllByRole('button', { name: /all tools/ })).toHaveLength(1);
  });
  it('puts a failed run’s cause and Retry at the failed turn, and shows a stopped chat’s pending tools as not run', async () => {
    const user = userEvent.setup(); const onRetryRun = vi.fn(); const base = createInitialState().chats[0];
    const failed: Message = { id: 'run', role: 'assistant', markdown: 'Partial reply', lifecycle: 'failed', note: 'Browser history result unavailable.', activity: [{ id: 'check', action: 'Check', detail: 'Return path', state: 'queued' }] };
    const view = render(<Conversation chat={{ ...base, status: 'failed', messages: [base.messages[0], failed] }} onRetryRun={onRetryRun} />);
    const turn = screen.getByRole('article', { name: 'AVA response' });
    expect(within(turn).getByRole('region', { name: 'Run status' })).toHaveTextContent('Failed');
    expect(within(turn).getAllByText('Browser history result unavailable.')).toHaveLength(1);
    expect(within(turn).getByRole('button', { name: 'Check Return path Not run' })).toBeVisible();
    await user.click(within(turn).getByRole('button', { name: 'Retry' })); expect(onRetryRun).toHaveBeenCalledTimes(1);
    const stopped = createInitialState().chats.find((entry) => entry.id === 'keyboard-navigation')!;
    view.rerender(<Conversation chat={{ ...stopped, status: 'stopped' }} />);
    expect(screen.getByRole('button', { name: /^Check focus order .* Not run$/ })).toBeVisible(); expect(screen.getByRole('button', { name: /^Read source .* Not finished$/ })).toBeVisible();
    expect(screen.queryByText('Queued')).not.toBeInTheDocument(); expect(screen.getByRole('region', { name: 'Run status' })).toHaveTextContent('Stopped');
  });
  it('gives a failure without Retry a next step: Ask AVA to retry on the run notice or on a failed tool’s cause', async () => {
    const user = userEvent.setup(); const onAskRetry = vi.fn(); const initial = createInitialState(); const auto = { ...defaultConversation, toolDisplay: 'auto' as const };
    const view = render(<Conversation chat={initial.chats.find((entry) => entry.id === 'retry-behavior')!} onAskRetry={onAskRetry} />);
    const notice = screen.getByRole('region', { name: 'Run status' });
    expect(notice).toHaveTextContent('AVA didn’t report why.');
    await user.click(within(notice).getByRole('button', { name: 'Ask AVA to retry' })); expect(onAskRetry).toHaveBeenCalledTimes(1);
    // A finished turn's failed tool carries it on its cause line.
    view.rerender(<Conversation chat={initial.chats[0]} onAskRetry={onAskRetry} preferences={auto} />);
    await user.click(await screen.findByRole('button', { name: 'Ask AVA to retry' })); expect(onAskRetry).toHaveBeenCalledTimes(2);
    // A failed turn that offers Retry keeps that as its one next step; its failed tools don't repeat it.
    const failed: Message = { id: 'run', role: 'assistant', markdown: 'Partial reply', lifecycle: 'failed', note: 'Browser history result unavailable.', activity: [{ id: 'check', action: 'Check', detail: 'Return path', state: 'failed' }] };
    view.rerender(<Conversation chat={{ ...initial.chats[0], status: 'failed', messages: [initial.chats[0].messages[0], failed] }} onRetryRun={vi.fn()} onAskRetry={onAskRetry} preferences={auto} />);
    expect(await screen.findByText('AVA didn’t report why this failed.')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Retry' })).toBeVisible(); expect(screen.queryByRole('button', { name: 'Ask AVA to retry' })).not.toBeInTheDocument();
    // While the run is still working, AVA may recover by itself.
    view.rerender(<Conversation chat={{ ...initial.chats[0], status: 'working' }} onAskRetry={onAskRetry} preferences={auto} />);
    expect(await screen.findByText('AVA didn’t report why this failed.')).toBeVisible(); expect(screen.queryByRole('button', { name: 'Ask AVA to retry' })).not.toBeInTheDocument();
  });
});
