import { StrictMode, useState } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { PermissionView, QuestionAnswer, QuestionView, RequestState } from '../contracts';
import { PermissionCard } from './PermissionCard';
import { HelpContext } from '../../components/helpContext';
import { QuestionCard } from './QuestionCard';

const permission: PermissionView = { kind: 'permission', id: 'opaque-request', version: 4, state: 'ready', title: 'Example action', summary: 'No execution authority.', decisions: ['allow', 'deny'], sessionAllowed: false };
const question: QuestionView = { kind: 'question', id: 'opaque-question', version: 1, state: 'ready', title: 'Review focus', description: 'Supplied options', selection: 'single', required: true, searchable: false, placement: 'inline', options: [{ id: 'a', label: 'Layout' }, { id: 'b', label: 'Keyboard' }], custom: { kind: 'none' } };
const empty: QuestionAnswer = { selectedIds: [], customText: '' };
const inactive: readonly RequestState[] = ['pending', 'submitted', 'error', 'stale', 'closed'];

describe('permission decision boundary', () => {
  it('has no default action, advertises only allowed choices, and emits an opaque versioned decision once with a receipt', async () => {
    const user = userEvent.setup();
    const onDecide = vi.fn();
    const { rerender } = render(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(onDecide).not.toHaveBeenCalled(); expect(document.activeElement).toBe(document.body);
    expect(screen.getByText('Needs your decision')).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Allow for this session' })).not.toBeInTheDocument();
    screen.getByRole('button', { name: 'Deny' }).focus();
    await user.keyboard(' ');
    fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    expect(onDecide).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-request', version: 4, decision: 'deny' });
    expect(screen.getByRole('status')).toHaveTextContent('You chose Deny.');
    expect(screen.getByRole('button', { name: 'Deny' })).toHaveAttribute('aria-pressed', 'true'); expect(screen.getByRole('button', { name: 'Deny' })).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Allow once' })).toHaveAttribute('aria-pressed', 'false'); expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    rerender(<PermissionCard request={{ ...permission, state: 'pending' }} onDecide={onDecide} />);
    rerender(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    rerender(<PermissionCard request={{ ...permission, version: 5, sessionAllowed: true, decisions: ['allow_session', 'deny'] }} onDecide={onDecide} />);
    expect(screen.getByRole('status')).toBeEmptyDOMElement(); expect(screen.getByRole('button', { name: 'Deny' })).not.toHaveAttribute('aria-pressed');
    await user.click(screen.getByRole('button', { name: 'Allow for this session' }));
    expect(onDecide).toHaveBeenLastCalledWith({ id: 'opaque-request', version: 5, decision: 'allow_session' });
    rerender(<PermissionCard request={permission} onDecide={onDecide} />);
    const deny = screen.getByRole('button', { name: 'Deny' });
    expect(deny).toHaveAttribute('aria-pressed', 'true'); expect(deny).toHaveAttribute('aria-disabled', 'true'); fireEvent.click(deny);
    expect(onDecide).toHaveBeenCalledTimes(2);
  });

  it('names the requester, the exact action and target, and one consequence line per choice; Deny sits apart and takes dialog focus', () => {
    render(<PermissionCard onDecide={vi.fn()} request={{ ...permission, sessionAllowed: true, decisions: ['allow', 'allow_session', 'deny'], requester: 'AVA', action: 'npm run export -- --from 2026-10-01', target: 'exports/range.csv', reason: 'Check the timezone boundary.', scopes: { allow: 'Only this command.' } }} />);
    expect(screen.getByText('Requested by AVA')).toBeVisible();
    expect(screen.getByText('npm run export -- --from 2026-10-01').tagName).toBe('CODE'); expect(screen.getByText('exports/range.csv').tagName).toBe('CODE');
    expect(screen.getByText('Check the timezone boundary.')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Allow once' })).toHaveAccessibleDescription('Only this command.');
    expect(screen.getByRole('button', { name: 'Allow for this session' })).toHaveAccessibleDescription('This kind of action in this chat until AVA restarts.');
    expect(screen.getByRole('button', { name: 'Deny' })).toHaveAccessibleDescription('AVA continues without doing this.');
    expect(screen.getByRole('button', { name: 'Deny' })).toHaveAttribute('data-autofocus'); expect(screen.getByRole('button', { name: 'Allow once' })).not.toHaveAttribute('data-autofocus');
    expect(screen.queryByText(/intent|host owns|policy/i)).not.toBeInTheDocument();
  });

  it('puts Deny first, so it is the first choice keyboard focus reaches, without giving any choice more weight', () => {
    render(<PermissionCard onDecide={vi.fn()} request={{ ...permission, sessionAllowed: true, decisions: ['allow', 'allow_session', 'deny'] }} />);
    const choices = within(screen.getByRole('group', { name: 'Your decision' })).getAllByRole('button');
    expect(choices.map((button) => button.textContent)).toEqual(['Deny', 'Allow once', 'Allow for this session']);
    expect(new Set(choices.map((button) => button.dataset.tone))).toEqual(new Set(['quiet']));
  });

  it('renders a stored decision as the receipt on a fresh mount and offers no second decision', () => {
    const onDecide = vi.fn();
    const { unmount } = render(<PermissionCard request={permission} decided="deny" onDecide={onDecide} />);
    expect(screen.getByRole('status')).toHaveTextContent('You chose Deny.'); expect(screen.getByText('Decided')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Deny' })).toHaveAttribute('aria-pressed', 'true'); expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Deny' })); fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    expect(onDecide).not.toHaveBeenCalled();
    unmount();
    // A stored choice this request never offered is not a receipt.
    render(<PermissionCard request={permission} decided="allow_session" onDecide={onDecide} />);
    expect(screen.getByRole('status')).toBeEmptyDOMElement(); expect(screen.getByRole('button', { name: 'Allow once' })).toBeEnabled();
  });

  it.each([['stale', 'Replaced by a newer request.'], ['closed', 'Closed by AVA · nothing was allowed.'], ['pending', 'Not ready for a decision yet.']] as const)('explains a %s request in a sentence', (state, message) => {
    render(<PermissionCard request={{ ...permission, state }} onDecide={vi.fn()} />);
    expect(screen.getByRole('status')).toHaveTextContent(message); expect(screen.queryByText('Needs your decision')).not.toBeInTheDocument();
  });

  it.each(inactive)('makes %s noninteractive even with a callback', (state) => {
    const onDecide = vi.fn();
    render(<PermissionCard request={{ ...permission, state }} onDecide={onDecide} />);
    screen.getAllByRole('button').forEach((button) => { expect(button).toBeDisabled(); fireEvent.click(button); });
    expect(onDecide).not.toHaveBeenCalled();
  });

  it.each([
    { id: '' }, { version: -1 }, { version: NaN }, { decisions: [] }, { decisions: ['allow', 'allow'] },
    { decisions: ['always'] }, { decisions: ['allow_session'] }, { state: 'unknown' },
  ])('fails closed for malformed permission metadata %j', (patch) => {
    const onDecide = vi.fn();
    render(<PermissionCard request={{ ...permission, ...patch } as PermissionView} onDecide={onDecide} />);
    expect(screen.getByRole('status')).toHaveTextContent('can’t be answered here');
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    expect(onDecide).not.toHaveBeenCalled();
  });

  it('does not offer authority without a callback and escapes supplied display strings', () => {
    const { container } = render(<PermissionCard request={{ ...permission, title: '<img src=x>', target: '<script>example</script>' }} />);
    expect(screen.getByText('<script>example</script>')).toBeInTheDocument();
    expect(container.querySelector('script, img')).toBeNull();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
  });

  it('links to About permissions after the decision, only where Help exists, without touching the decision', async () => {
    const user = userEvent.setup(); const onDecide = vi.fn(); const openHelp = vi.fn();
    const { unmount } = render(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(screen.queryByRole('button', { name: 'About permissions' })).not.toBeInTheDocument();
    unmount();
    render(<HelpContext.Provider value={openHelp}><PermissionCard request={{ ...permission, sessionAllowed: true, decisions: ['allow', 'allow_session', 'deny'] }} onDecide={onDecide} /></HelpContext.Provider>);
    const link = screen.getByRole('button', { name: 'About permissions' });
    // Deny stays the first control keyboard focus reaches; the link comes after every choice.
    await user.tab(); expect(screen.getByRole('button', { name: 'Deny' })).toHaveFocus();
    const order = screen.getAllByRole('button').map((button) => button.textContent);
    expect(order).toEqual(['Deny', 'Allow once', 'Allow for this session', 'About permissions']);
    expect(within(screen.getByRole('group', { name: 'Your decision' })).queryByRole('button', { name: 'About permissions' })).not.toBeInTheDocument();
    await user.click(link);
    expect(openHelp).toHaveBeenCalledExactlyOnceWith('permissions'); expect(onDecide).not.toHaveBeenCalled();
    expect(screen.getByRole('status')).toBeEmptyDOMElement(); expect(screen.getByRole('button', { name: 'Deny' })).toBeEnabled();
  });
});

describe('controlled question forms', () => {
  it('uses native keyboard choice, controlled values, required validation, and one versioned answer', async () => {
    const user = userEvent.setup();
    const onAnswer = vi.fn();
    function Example() {
      const [value, setValue] = useState(empty);
      return <QuestionCard request={question} value={value} onChange={setValue} onAnswer={onAnswer} />;
    }
    render(<Example />);
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Choose an answer to continue.');
    expect(onAnswer).not.toHaveBeenCalled();
    screen.getByRole('radio', { name: 'Layout' }).focus();
    await user.keyboard(' ');
    expect(screen.getByRole('radio', { name: 'Layout' })).toBeChecked();
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-question', version: 1, answer: { selectedIds: ['a'], customText: '' } });
    expect(screen.getByRole('radio', { name: 'Keyboard' })).toBeDisabled();
  });

  it('retains multiple selections while filtering and does not invent values before parent updates', async () => {
    const user = userEvent.setup();
    const onChange = vi.fn();
    const request: QuestionView = { ...question, selection: 'multiple', searchable: true };
    const { rerender } = render(<QuestionCard request={request} value={empty} onChange={onChange} />);
    await user.click(screen.getByRole('checkbox', { name: 'Layout' }));
    expect(onChange).toHaveBeenLastCalledWith({ selectedIds: ['a'], customText: '' });
    expect(screen.getByRole('checkbox', { name: 'Layout' })).not.toBeChecked();
    rerender(<QuestionCard request={request} value={{ selectedIds: ['a'], customText: '' }} onChange={onChange} />);
    await user.type(screen.getByRole('searchbox'), 'Keyboard');
    await user.click(screen.getByRole('checkbox', { name: 'Keyboard' }));
    expect(onChange).toHaveBeenLastCalledWith({ selectedIds: ['a', 'b'], customText: '' });
    expect(screen.queryByRole('checkbox', { name: 'Layout' })).not.toBeInTheDocument();
  });

  it.each([
    { selectedIds: ['missing'], customText: '' }, { selectedIds: ['a', 'a'], customText: '' },
    { selectedIds: ['a', 'b'], customText: '' }, { selectedIds: [], customText: 'not allowed' },
    { selectedIds: ['a'], customText: 'not allowed' }, { selectedIds: null, customText: '' },
    { selectedIds: [], customText: null },
  ])('rejects invalid or duplicate answer input %j', (value) => {
    const onAnswer = vi.fn();
    render(<QuestionCard request={question} value={value as QuestionAnswer} onChange={vi.fn()} onAnswer={onAnswer} />);
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).not.toHaveBeenCalled();
  });

  it.each([
    { options: [{ id: 'a', label: 'A' }, { id: 'a', label: 'Again' }] }, { options: [] },
    { options: [{ id: '', label: 'Empty' }] }, { options: [{ id: 'a', label: '' }] }, { id: '' }, { version: 1.5 },
    { placement: 'modal' }, { selection: 'arbitrary' }, { custom: { kind: 'unsupported' } },
    { custom: { kind: 'secret', label: '' } }, { searchable: 'yes' }, { required: 'yes' },
  ])('fails closed for unsupported question metadata %j', (patch) => {
    const onAnswer = vi.fn();
    render(<QuestionCard request={{ ...question, ...patch } as QuestionView} value={empty} onChange={vi.fn()} onAnswer={onAnswer} />);
    expect(screen.getByRole('status')).toHaveTextContent('can’t be answered here');
    expect(screen.getByRole('button', { name: 'Submit answer' })).toBeDisabled();
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).not.toHaveBeenCalled();
  });

  it.each(inactive.filter((state) => state !== 'error'))('blocks answer, cancel, and input callbacks while %s', (state) => {
    const onAnswer = vi.fn(); const onChange = vi.fn(); const onCancel = vi.fn();
    render(<QuestionCard request={{ ...question, state }} value={{ selectedIds: ['a'], customText: '' }} onChange={onChange} onAnswer={onAnswer} onCancel={onCancel} />);
    const radio = screen.getByRole('radio', { name: 'Keyboard' });
    expect(radio).toBeDisabled();
    fireEvent.click(radio);
    fireEvent.submit(screen.getByRole('form'));
    fireEvent.click(screen.getByRole('button', { name: 'Cancel question' }));
    expect(onAnswer).not.toHaveBeenCalled(); expect(onChange).not.toHaveBeenCalled(); expect(onCancel).not.toHaveBeenCalled();
  });

  it('replaces a failed question’s choices and actions with its cause, and sends one Retry per version', async () => {
    const user = userEvent.setup(); const onRetry = vi.fn(); const onAnswer = vi.fn(); const onCancel = vi.fn();
    const failed: QuestionView = { ...question, state: 'error', error: 'This question couldn’t be loaded.' };
    const props = { value: { selectedIds: ['a'], customText: '' }, onChange: vi.fn(), onAnswer, onCancel };
    const { rerender } = render(<QuestionCard request={failed} {...props} onRetry={onRetry} />);
    expect(screen.getByRole('status')).toHaveTextContent(/Failed/); expect(screen.getByRole('status')).toHaveTextContent('This question couldn’t be loaded.');
    expect(screen.queryByRole('radio')).not.toBeInTheDocument(); expect(screen.queryByRole('form')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Submit answer' })).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: 'Cancel question' })).not.toBeInTheDocument();
    const retry = screen.getByRole('button', { name: 'Retry' });
    await user.click(retry); await user.click(retry);
    expect(onRetry).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-question', version: 1 });
    expect(retry).toHaveAttribute('aria-disabled', 'true'); expect(retry).toHaveFocus(); expect(screen.getByRole('status')).toHaveTextContent('Retry requested.');
    rerender(<QuestionCard request={{ ...failed, version: 2 }} {...props} onRetry={onRetry} />);
    await user.click(screen.getByRole('button', { name: 'Retry' })); expect(onRetry).toHaveBeenLastCalledWith({ id: 'opaque-question', version: 2 });
    rerender(<QuestionCard request={{ ...failed, version: 3, error: undefined }} {...props} />);
    expect(screen.queryByRole('button')).not.toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('This question couldn’t be completed.');
    expect(onAnswer).not.toHaveBeenCalled(); expect(onCancel).not.toHaveBeenCalled(); expect(onRetry).toHaveBeenCalledTimes(2);
  });

  it('masks custom secrets, never echoes them, and clears single-choice IDs for a custom answer', async () => {
    const user = userEvent.setup();
    const onAnswer = vi.fn();
    function Example() {
      const [value, setValue] = useState<QuestionAnswer>({ selectedIds: ['a'], customText: '' });
      return <QuestionCard request={{ ...question, custom: { kind: 'secret', label: 'Secret answer' } }} value={value} onChange={setValue} onAnswer={onAnswer} />;
    }
    const { container } = render(<Example />);
    const input = screen.getByLabelText('Secret answer');
    expect(input).toHaveAttribute('type', 'password');
    await user.type(input, 'invented-sensitive-answer');
    expect(screen.getByRole('radio', { name: 'Layout' })).not.toBeChecked(); expect(screen.getByRole('radio', { name: 'Other…' })).toBeChecked();
    await user.keyboard('{Enter}');
    expect(onAnswer).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-question', version: 1, answer: { selectedIds: [], customText: 'invented-sensitive-answer' } });
    expect(container.textContent).not.toContain('invented-sensitive-answer');
    expect(input).not.toHaveAttribute('aria-label', expect.stringContaining('invented-sensitive-answer'));
  });

  it('keeps cancel explicit, blocks duplicates across version switches, and leaves Escape/native focus alone', async () => {
    const user = userEvent.setup();
    const onCancel = vi.fn(); const onAnswer = vi.fn(); const onChange = vi.fn();
    const props = { value: { selectedIds: ['a'], customText: '' }, onChange, onCancel, onAnswer };
    const { rerender, unmount } = render(<StrictMode><QuestionCard request={question} {...props} /></StrictMode>);
    const radio = screen.getByRole('radio', { name: 'Layout' });
    radio.focus(); await user.keyboard('{Escape}');
    expect(radio).toHaveFocus(); expect(onCancel).not.toHaveBeenCalled();
    await user.click(screen.getByRole('button', { name: 'Cancel question' }));
    fireEvent.submit(screen.getByRole('form'));
    expect(onCancel).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-question', version: 1 });
    expect(onAnswer).not.toHaveBeenCalled();
    rerender(<StrictMode><QuestionCard request={{ ...question, version: 2 }} {...props} /></StrictMode>);
    expect(screen.getByRole('button', { name: 'Submit answer' })).toBeEnabled();
    rerender(<StrictMode><QuestionCard request={question} {...props} /></StrictMode>);
    const button = screen.getByRole('button', { name: 'Submit answer' });
    expect(button).toBeDisabled();
    unmount(); fireEvent.click(button);
    expect(onAnswer).not.toHaveBeenCalled();
  });

  it('accepts a deliberately optional empty answer and a supported plain custom answer', () => {
    const onAnswer = vi.fn();
    const { rerender } = render(<QuestionCard request={{ ...question, required: false }} value={empty} onChange={vi.fn()} onAnswer={onAnswer} />);
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).toHaveBeenCalledTimes(1);
    rerender(<QuestionCard request={{ ...question, version: 2, custom: { kind: 'text', label: 'Other' } }} value={{ selectedIds: [], customText: 'Another focus' }} onChange={vi.fn()} onAnswer={onAnswer} />);
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).toHaveBeenLastCalledWith({ id: question.id, version: 2, answer: { selectedIds: [], customText: 'Another focus' } });
  });

  it('makes choosing Other… visible and asks for its text before sending', async () => {
    const user = userEvent.setup(); const onAnswer = vi.fn();
    function Example() {
      const [value, setValue] = useState<QuestionAnswer>({ selectedIds: ['a'], customText: '' });
      return <QuestionCard request={{ ...question, custom: { kind: 'text', label: 'Your answer' } }} value={value} onChange={setValue} onAnswer={onAnswer} />;
    }
    render(<Example />);
    await user.click(screen.getByRole('radio', { name: 'Other…' }));
    expect(screen.getByRole('radio', { name: 'Other…' })).toBeChecked(); expect(screen.getByRole('radio', { name: 'Layout' })).not.toBeChecked(); expect(screen.getByLabelText('Your answer')).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Type your answer, or choose one of the options.'); expect(screen.getByLabelText('Your answer')).toHaveAttribute('aria-invalid', 'true');
    expect(onAnswer).not.toHaveBeenCalled();
    await user.type(screen.getByLabelText('Your answer'), 'Copy'); expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(onAnswer).toHaveBeenCalledExactlyOnceWith({ id: question.id, version: 1, answer: { selectedIds: [], customText: 'Copy' } });
  });

  it('shows a secret-only question as one labelled field without an options legend', async () => {
    const user = userEvent.setup(); const onAnswer = vi.fn();
    function Example() {
      const [value, setValue] = useState(empty);
      return <QuestionCard request={{ ...question, options: [], custom: { kind: 'secret', label: 'Access code' } }} value={value} onChange={setValue} onAnswer={onAnswer} />;
    }
    const { container } = render(<Example />);
    expect(container.querySelector('legend')).toBeNull(); expect(screen.queryByRole('radio')).not.toBeInTheDocument();
    expect(screen.getByLabelText('Access code')).toHaveAttribute('type', 'password');
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Type an answer to continue.'); expect(onAnswer).not.toHaveBeenCalled();
  });
});
