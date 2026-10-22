import { useState } from 'react';
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { FinderView, WorkspaceIntent, WorkspaceView } from './contracts';
import { ChatActionDialog, PermissionDialog, QuestionDialog, RenameDialog, validTitle, validWorkspaceIntent, WorkspaceDialog } from './ChatDialogs';
import { installDialogTestSupport } from './dialogTestSupport';
import { FinderDialog, validFinderIntent } from './FinderDialog';
import { branchPoints, permissionCase, projects, questionCase, sessions, workspaceChoices } from './fixtures';

beforeEach(installDialogTestSupport);
const chrome = { onDismiss: vi.fn() };
const chat = { id: 'chat', version: 1, title: 'Refine search filters', state: 'ready' as const };

describe('chat finder', () => {
  it('separates archived chats and refuses unbound or stale finder choices', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const view: FinderView = { id: 'sessions', version: 1, kind: 'sessions', state: 'ready', items: sessions, projects };
    render(<FinderDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    await user.click(screen.getByRole('button', { name: 'Archived' })); expect(screen.queryByRole('option', { name: 'Refine search filters' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('option', { name: 'Organize reading notes' })); expect(onSelect).toHaveBeenCalledWith({ id: 'sessions', version: 1, itemId: 'notes' });
    expect(validFinderIntent(view, { id: 'sessions', version: 0, itemId: 'notes' })).toBe(false);
    expect(validFinderIntent(view, { id: 'sessions', version: 1, itemId: 'release' })).toBe(false);
    expect(validFinderIntent({ ...view, kind: 'commands' }, { id: 'sessions', version: 1, itemId: 'notes' })).toBe(false);
  });
  it('opens on a requested filter that the filter buttons still change', async () => {
    const user = userEvent.setup(); const view: FinderView = { id: 'sessions', version: 1, kind: 'sessions', state: 'ready', items: sessions, projects };
    render(<FinderDialog {...chrome} view={view} initialScope="archived" query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(screen.getByRole('button', { name: 'Archived' })).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getAllByRole('option').map((option) => option.textContent)).toEqual([expect.stringContaining('Organize reading notes')]);
    await user.click(screen.getByRole('button', { name: 'All' }));
    expect(screen.getByRole('option', { name: 'Refine search filters' })).toBeInTheDocument();
  });
  it('finds chats by their messages, shows where the words appear, and keeps a plain no-match notice', () => {
    const view: FinderView = { id: 'sessions', version: 1, kind: 'sessions', state: 'ready', items: sessions, projects };
    const props = { ...chrome, view, onQueryChange: vi.fn(), onSelect: vi.fn(), onRetry: vi.fn() };
    const result = render(<FinderDialog {...props} query="reading timezone" />);
    expect(screen.getAllByRole('option')).toHaveLength(1);
    const notes = screen.getByRole('option', { name: 'Organize reading notes' });
    // Only the word the title doesn't cover is shown in context and marked.
    expect(notes).toHaveAccessibleDescription(/…Dates follow the reader’s timezone, newest first\.$/);
    expect(Array.from(notes.querySelectorAll('mark'), (mark) => mark.textContent)).toEqual(['timezone']);
    result.rerender(<FinderDialog {...props} query="keyboard" />);
    expect(screen.getByRole('option', { name: 'Keyboard navigation' }).querySelector('mark')).toBeNull();
    expect(screen.getByRole('option', { name: 'Refine search filters' }).querySelector('mark')).toBeNull();
    result.rerender(<FinderDialog {...props} query="zebra" />);
    expect(screen.queryByRole('option')).not.toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent('No chats match “zebra”.');
  });
});

describe('rename', () => {
  it('selects the current title, counts only near the limit and validates empty and too-long titles', async () => {
    const user = userEvent.setup(); const onRename = vi.fn();
    const result = render(<RenameDialog {...chrome} view={chat} onRename={onRename} />);
    const input = screen.getByLabelText<HTMLInputElement>('Chat title');
    expect(input).toHaveFocus(); expect([input.selectionStart, input.selectionEnd]).toEqual([0, chat.title.length]);
    expect(input).toHaveAttribute('maxlength', '256'); expect(screen.queryByText(/characters? left/)).not.toBeInTheDocument();
    await user.clear(input); await user.click(screen.getByRole('button', { name: 'Rename' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Enter a title for this chat.'); expect(input).toHaveAttribute('aria-invalid', 'true'); expect(onRename).not.toHaveBeenCalled();
    await user.type(input, '  Better title  '); await user.click(screen.getByRole('button', { name: 'Rename' }));
    expect(onRename).toHaveBeenCalledExactlyOnceWith({ id: 'chat', version: 1, title: 'Better title' });
    result.rerender(<RenameDialog {...chrome} view={{ ...chat, version: 2, title: 'A'.repeat(220) }} onRename={onRename} />);
    expect(screen.getByLabelText('Chat title')).toHaveAccessibleDescription('36 characters left');
    result.rerender(<RenameDialog {...chrome} view={{ ...chat, version: 3, title: 'A'.repeat(257) }} onRename={onRename} />);
    expect(screen.getByRole('alert')).toHaveTextContent('Titles can be up to 256 characters. Remove 1 character.');
    await user.click(screen.getByRole('button', { name: 'Rename' })); expect(onRename).toHaveBeenCalledTimes(1);
    expect(validTitle('A'.repeat(256))).toBe(true); expect(validTitle('A'.repeat(257))).toBe(false); expect(validTitle('   ')).toBe(false);
  });
  it('explains why a running chat can’t be renamed yet', () => {
    render(<RenameDialog {...chrome} view={{ ...chat, state: 'busy' }} onRename={vi.fn()} />);
    expect(screen.getByRole('status')).toHaveTextContent('You can rename this chat after its run finishes or is stopped.');
    expect(screen.getByLabelText('Chat title')).toBeDisabled(); expect(screen.getByRole('button', { name: 'Rename' })).toBeDisabled();
  });
});

describe('archive and branch', () => {
  it('states what archiving does and confirms with a neutral primary that takes focus', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn();
    const result = render(<ChatActionDialog onDismiss={onDismiss} action="archive" view={chat} points={[]} onIntent={onIntent} onGoToChat={vi.fn()} />);
    const dialog = screen.getByRole('dialog', { name: 'Archive chat?' });
    expect(dialog).toHaveAccessibleDescription('Refine search filters');
    expect(dialog).toHaveTextContent('Removed from Projects, Pinned and Recent.');
    expect(dialog).toHaveTextContent('Nothing is deleted. Find it in Find a chat → Archived.');
    expect(dialog).not.toHaveTextContent(/RPC|intent|example|preview/i);
    const confirm = within(dialog).getByRole('button', { name: 'Archive chat' });
    expect(confirm).toHaveAttribute('data-tone', 'primary'); expect(confirm).toHaveFocus();
    expect(within(dialog).queryByRole('button', { name: 'Go to chat' })).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('button', { name: 'Cancel' })); expect(onDismiss).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
    result.rerender(<ChatActionDialog key="next" onDismiss={onDismiss} action="archive" view={{ ...chat, version: 2 }} points={[]} onIntent={onIntent} />);
    await user.click(screen.getByRole('button', { name: 'Archive chat' }));
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'chat', version: 2, action: 'archive' });
  });
  it('links the guidance to Find a chat on Archived, with its key, and follows it once', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onFindArchived = vi.fn();
    render(<ChatActionDialog {...chrome} action="archive" view={chat} points={[]} onIntent={onIntent} onFindArchived={onFindArchived} findShortcut="Ctrl+P" />);
    const dialog = screen.getByRole('dialog', { name: 'Archive chat?' });
    expect(dialog).toHaveTextContent('Nothing is deleted. Find it in Find a chat Ctrl+P → Archived.');
    expect(within(dialog).getByRole('button', { name: 'Archive chat' })).toHaveFocus();
    const link = within(dialog).getByRole('button', { name: 'Find a chat, Archived' });
    await user.click(link); await user.click(link);
    expect(onFindArchived).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
  });
  it('asks to stop a running chat first and offers the way back to it', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onGoToChat = vi.fn();
    render(<ChatActionDialog {...chrome} action="archive" view={{ ...chat, state: 'busy' }} points={[]} onIntent={onIntent} onGoToChat={onGoToChat} />);
    expect(screen.getByRole('status')).toHaveTextContent('This chat is still running. Stop the run first, then archive it.');
    expect(screen.getByRole('button', { name: 'Archive chat' })).toBeDisabled();
    const back = screen.getByRole('button', { name: 'Go to chat' }); expect(back).toHaveFocus();
    await user.click(back); await user.click(back);
    expect(onGoToChat).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
  });
  it('preselects the newest message, says what each branch keeps, and submits only from the button', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<ChatActionDialog {...chrome} action="branch" view={chat} points={branchPoints} onIntent={onIntent} />);
    expect(screen.getByRole('dialog', { name: 'Branch from a message' })).toBeVisible();
    const radios = screen.getAllByRole<HTMLInputElement>('radio');
    expect(radios.map((radio) => radio.value)).toEqual(['follow-up', 'labels', 'request']);
    expect(radios[0]).toBeChecked(); expect(radios[0]).toHaveFocus();
    expect(radios[0].closest('label')).toHaveTextContent('Message 3 of 3'); expect(radios[0].closest('label')).toHaveTextContent('Keeps the whole chat up to this message.');
    expect(radios[2].closest('label')).toHaveTextContent('Message 1 of 3'); expect(radios[2].closest('label')).toHaveTextContent('The 2 later messages stay in the original.');
    await user.keyboard('{ArrowRight}'); expect(radios[1]).toBeChecked();
    await user.keyboard('{Enter}'); expect(onIntent).not.toHaveBeenCalled(); expect(screen.getByRole('dialog')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Create branch' }));
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'chat', version: 1, action: 'branch', pointId: 'labels' });
  });
  it('shows the picked message in full where its one-line label was, and keeps each radio name to that line', async () => {
    const user = userEvent.setup();
    const whole = 'Keep the search filters when I open a result and go back.\nUse the URL so I can share the same view.';
    const points = [{ id: 'scan', label: 'Make the search filters easier to scan.' }, { id: 'return', label: 'Keep the search filters when I open a result and go back…', text: `  ${whole}\n` }];
    render(<ChatActionDialog {...chrome} action="branch" view={chat} points={points} onIntent={vi.fn()} />);
    const [newest, oldest] = screen.getAllByRole<HTMLInputElement>('radio');
    expect(newest).toBeChecked(); expect(newest).toHaveFocus();
    expect(newest).toHaveAccessibleName('Message 2 of 2 Keep the search filters when I open a result and go back…');
    expect(newest).toHaveAccessibleDescription('Keeps the whole chat up to this message.');
    const full = screen.getByRole('region', { name: 'Message 2 in full' });
    expect(full.textContent).toBe(whole); expect(full).toHaveAttribute('tabindex', '0');
    expect(newest.closest('label')).toContainElement(full);
    // A message its label already says in full has nothing more to show.
    await user.keyboard('{ArrowRight}'); expect(oldest).toBeChecked();
    expect(screen.queryByRole('region', { name: /in full$/ })).not.toBeInTheDocument();
    expect(oldest.closest('label')).toHaveTextContent('Make the search filters easier to scan.');
    expect(oldest).toHaveAccessibleDescription('Keeps the chat up to this message. The 1 later message stays in the original.');
  });
  it('shows one plain notice and no orphan legend when there is nothing to branch from', () => {
    render(<ChatActionDialog {...chrome} action="branch" view={chat} points={[]} onIntent={vi.fn()} />);
    expect(screen.getByRole('status')).toHaveTextContent('This chat has no messages to branch from yet.');
    expect(screen.queryByRole('group')).not.toBeInTheDocument(); expect(screen.queryByText('Your messages')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Create branch' })).toBeDisabled();
  });
});

describe('workspace', () => {
  const base: WorkspaceView = { id: 'workspace', version: 1, state: 'choice', selectedId: 'atlas', choices: workspaceChoices };
  it('rejects unbound, duplicate, stale and out-of-state workspace intents', () => {
    const intent = (action: WorkspaceIntent['action'], workspaceId?: string): WorkspaceIntent => ({ id: 'workspace', version: 1, action, workspaceId });
    expect(validWorkspaceIntent(base, intent('choose', 'unknown'))).toBe(false);
    expect(validWorkspaceIntent(base, { ...intent('choose', 'atlas'), version: 0 })).toBe(false);
    expect(validWorkspaceIntent({ ...base, choices: [workspaceChoices[0], workspaceChoices[0]] }, intent('choose', 'atlas'))).toBe(false);
    expect(validWorkspaceIntent({ ...base, state: 'ready', selectedId: 'unknown' }, intent('use'))).toBe(false);
    expect(validWorkspaceIntent({ ...base, state: 'opening' }, intent('use'))).toBe(false);
    expect(validWorkspaceIntent(base, intent('change'))).toBe(false);
    expect(validWorkspaceIntent({ ...base, state: 'ready' }, intent('use'))).toBe(true);
    expect(validWorkspaceIntent({ ...base, state: 'error' }, intent('change'))).toBe(true);
    expect(validWorkspaceIntent({ ...base, state: 'opening' }, intent('cancel'))).toBe(true);
  });
  it('recovers from a failure by retrying or choosing another project, with one way to cancel opening', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    function Host() {
      const [view, setView] = useState<WorkspaceView>({ ...base, state: 'error' });
      return <WorkspaceDialog view={view} onDismiss={vi.fn()} onIntent={(intent) => {
        onIntent(intent.action);
        const state = intent.action === 'change' ? 'choice' : intent.action === 'cancel' ? 'canceled' : intent.action === 'use' ? view.state : 'opening';
        setView({ ...view, version: view.version + 1, state, selectedId: intent.workspaceId ?? view.selectedId });
      }} />;
    }
    render(<Host />);
    expect(screen.getByRole('dialog', { name: 'Open a project' })).toBeVisible();
    expect(screen.getByRole('alert')).toHaveTextContent('Couldn’t open'); expect(screen.getByRole('alert')).toHaveTextContent('Retry, or choose another project.');
    expect(screen.getByRole('button', { name: 'Retry' })).toHaveAttribute('data-tone', 'primary');
    expect(screen.queryByText(/Folder picker|example|workspace/i)).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Choose another project' }));
    expect(screen.getByRole('button', { name: /^Atlas/ })).toHaveTextContent('Current');
    await user.click(screen.getByRole('button', { name: /^Notebook/ }));
    expect(screen.getByRole('status')).toHaveTextContent('Notebook'); expect(screen.getByRole('status')).toHaveTextContent('Opening');
    const actions = screen.getAllByRole('button').filter((button) => button.textContent && button.getAttribute('aria-label') !== 'Close');
    expect(actions.map((button) => button.textContent)).toEqual(['Cancel opening']);
    await user.click(screen.getByRole('button', { name: 'Cancel opening' }));
    expect(screen.getByRole('status')).toHaveTextContent('Canceled');
    expect(screen.getByRole('button', { name: 'Open Notebook' })).toBeEnabled();
    expect(onIntent.mock.calls.flat()).toEqual(['change', 'choose', 'cancel']);
  });
  it('opens on the current project and moves between projects with arrow keys, Home and End', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    const choices = [...workspaceChoices, { id: 'archive', label: 'Archive', detail: '~/Archive' }];
    render(<WorkspaceDialog {...chrome} view={{ ...base, selectedId: 'notebook', choices }} onIntent={onIntent} />);
    const row = (name: RegExp) => within(screen.getByRole('list', { name: 'Projects' })).getByRole('button', { name });
    expect(row(/^Notebook/)).toHaveFocus(); expect(row(/^Notebook/)).toHaveAttribute('aria-current', 'true');
    await user.keyboard('{ArrowDown}'); expect(row(/^Archive/)).toHaveFocus();
    await user.keyboard('{ArrowDown}'); expect(row(/^Atlas/)).toHaveFocus();
    await user.keyboard('{ArrowUp}'); expect(row(/^Archive/)).toHaveFocus();
    await user.keyboard('{Home}'); expect(row(/^Atlas/)).toHaveFocus();
    await user.keyboard('{End}'); expect(row(/^Archive/)).toHaveFocus();
    await user.keyboard('{Enter}');
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'workspace', version: 1, action: 'choose', workspaceId: 'archive' });
  });
  it('offers the ready project as the primary action', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<WorkspaceDialog {...chrome} view={{ ...base, state: 'ready' }} onIntent={onIntent} />);
    expect(screen.getByRole('status')).toHaveTextContent('Ready');
    await user.click(screen.getByRole('button', { name: 'Use Atlas' }));
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'workspace', version: 1, action: 'use', workspaceId: undefined });
  });
});

describe('request dialogs', () => {
  it('titles the dialog with the request, starts on Deny, leaves dismissal undecided and guards a double decision', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn();
    const result = render(<PermissionDialog onDismiss={onDismiss} request={permissionCase('once', 1)} onIntent={onIntent} />);
    const dialog = screen.getByRole('dialog', { name: 'Run the search filter tests?' });
    expect(within(dialog).getAllByRole('heading')).toHaveLength(1);
    expect(dialog).toHaveAccessibleDescription('Closing leaves this request waiting for your decision.');
    expect(within(dialog).getByRole('button', { name: 'Deny' })).toHaveFocus();
    expect(dialog).toHaveTextContent('npm test -- search-filters');
    expect(screen.queryByRole('button', { name: 'Allow for this session' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Decide later' })); expect(onIntent).not.toHaveBeenCalled(); expect(onDismiss).toHaveBeenCalled();
    result.rerender(<PermissionDialog onDismiss={onDismiss} request={permissionCase('session', 2)} onIntent={onIntent} />);
    await user.dblClick(screen.getByRole('button', { name: 'Allow for this session' }));
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'review-permission', version: 2, kind: 'permission', decision: 'allow_session' });
    expect(screen.queryByText(/Allow always/i)).not.toBeInTheDocument();
  });
  it.each(['pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'])('fails closed for %s requests', (scenario) => {
    const onIntent = vi.fn(); render(<PermissionDialog {...chrome} request={permissionCase(scenario, 1)} onIntent={onIntent} />);
    for (const button of within(screen.getByRole('dialog')).queryAllByRole('button', { name: /Allow once|Allow for this session|^Deny$/ })) expect(button).toBeDisabled(); expect(onIntent).not.toHaveBeenCalled();
  });
  it('titles a question dialog with the question and starts on the first answer', () => {
    render(<QuestionDialog {...chrome} request={questionCase('single', 1)} onIntent={vi.fn()} />);
    const dialog = screen.getByRole('dialog', { name: 'How should the result be presented?' });
    expect(within(dialog).getAllByRole('heading')).toHaveLength(1);
    expect(within(dialog).getAllByRole('radio')[0]).toHaveFocus();
  });
  it('offers Retry for a question that failed to load, only when the host can reload it, and asks once', async () => {
    const user = userEvent.setup(); const onRetry = vi.fn(); const onIntent = vi.fn();
    const result = render(<QuestionDialog {...chrome} request={questionCase('error', 1)} onIntent={onIntent} />);
    expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    result.rerender(<QuestionDialog {...chrome} request={questionCase('error', 1)} onIntent={onIntent} onRetry={onRetry} />);
    const dialog = screen.getByRole('dialog'); expect(within(dialog).queryByRole('radio')).not.toBeInTheDocument();
    await user.dblClick(within(dialog).getByRole('button', { name: 'Retry' }));
    expect(onRetry).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
  });
  it('clears secret drafts when the supplied state goes stale without changing identity', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const request = questionCase('secret', 1);
    const result = render(<QuestionDialog {...chrome} request={request} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Access token'), 'ephemeral-demo');
    result.rerender(<QuestionDialog {...chrome} request={{ ...request, state: 'stale' }} onIntent={onIntent} />);
    expect(screen.getByLabelText('Access token')).toHaveValue(''); expect(screen.getByRole('button', { name: 'Submit answer' })).toBeDisabled(); expect(onIntent).not.toHaveBeenCalled();
  });
  it('cancel clears a custom answer and emits cancellation only', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<QuestionDialog {...chrome} request={questionCase('custom', 1)} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Describe the format'), 'unrecorded custom text'); await user.click(screen.getByRole('button', { name: 'Cancel question' }));
    expect(screen.getByLabelText('Describe the format')).toHaveValue(''); expect(onIntent).toHaveBeenCalledWith({ id: 'review-question', version: 1, kind: 'question-cancel' });
  });
  it('clears a masked answer and excludes custom/secret content from receipts', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const result = render(<QuestionDialog {...chrome} request={questionCase('secret', 1)} onIntent={onIntent} />);
    const input = screen.getByLabelText('Access token'); expect(input).toHaveAttribute('type', 'password'); await user.type(input, 'dummy-only-value'); await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(input).toHaveValue(''); expect(onIntent).toHaveBeenCalledWith({ id: 'review-question', version: 1, kind: 'question-answer' }); expect(JSON.stringify(onIntent.mock.calls)).not.toContain('dummy-only-value');
    result.rerender(<QuestionDialog {...chrome} request={questionCase('secret', 2)} onIntent={onIntent} />); expect(screen.getByLabelText('Access token')).toHaveValue('');
  });
});
