import { StrictMode, useState } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { PermissionView, QuestionAnswer, QuestionView, RequestState } from '../contracts';
import { PermissionCard } from './PermissionCard';
import { QuestionCard } from './QuestionCard';

const permission: PermissionView = { kind: 'permission', id: 'opaque-request', version: 4, state: 'ready', title: 'Example action', summary: 'No execution authority.', decisions: ['allow', 'deny'], sessionAllowed: false };
const question: QuestionView = { kind: 'question', id: 'opaque-question', version: 1, state: 'ready', title: 'Review focus', description: 'Supplied options', selection: 'single', required: true, searchable: false, placement: 'inline', options: [{ id: 'a', label: 'Layout' }, { id: 'b', label: 'Keyboard' }], custom: { kind: 'none' } };
const empty: QuestionAnswer = { selectedIds: [], customText: '' };
const inactive: readonly RequestState[] = ['pending', 'submitted', 'error', 'stale', 'closed'];

describe('permission UI intent boundary', () => {
  it('has no default action, advertises only allowed choices, and emits an opaque versioned intent once', async () => {
    const user = userEvent.setup();
    const onDecide = vi.fn();
    const { rerender } = render(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(onDecide).not.toHaveBeenCalled();
    expect(screen.queryByRole('button', { name: 'Allow for session' })).not.toBeInTheDocument();
    screen.getByRole('button', { name: 'Deny' }).focus();
    await user.keyboard(' ');
    fireEvent.click(screen.getByRole('button', { name: 'Allow once' }));
    expect(onDecide).toHaveBeenCalledExactlyOnceWith({ id: 'opaque-request', version: 4, decision: 'deny' });
    expect(screen.getByRole('status')).toHaveTextContent('Intent submitted');
    rerender(<PermissionCard request={{ ...permission, state: 'pending' }} onDecide={onDecide} />);
    rerender(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    rerender(<PermissionCard request={{ ...permission, version: 5, sessionAllowed: true, decisions: ['allow_session', 'deny'] }} onDecide={onDecide} />);
    await user.click(screen.getByRole('button', { name: 'Allow for session' }));
    expect(onDecide).toHaveBeenLastCalledWith({ id: 'opaque-request', version: 5, decision: 'allow_session' });
    rerender(<PermissionCard request={permission} onDecide={onDecide} />);
    expect(screen.getByRole('button', { name: 'Deny' })).toBeDisabled();
    expect(onDecide).toHaveBeenCalledTimes(2);
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
    expect(screen.getByRole('status')).toHaveTextContent('Unsupported or invalid request');
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    expect(onDecide).not.toHaveBeenCalled();
  });

  it('does not offer authority without a callback and escapes supplied display strings', () => {
    const { container } = render(<PermissionCard request={{ ...permission, title: '<img src=x>', target: '<script>example</script>' }} />);
    expect(screen.getByText('<script>example</script>')).toBeInTheDocument();
    expect(container.querySelector('script, img')).toBeNull();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
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
    expect(screen.getByRole('alert')).toHaveTextContent('Choose a valid answer');
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
    expect(screen.getByRole('status')).toHaveTextContent('Unsupported or invalid request');
    expect(screen.getByRole('button', { name: 'Submit answer' })).toBeDisabled();
    fireEvent.submit(screen.getByRole('form'));
    expect(onAnswer).not.toHaveBeenCalled();
  });

  it.each(inactive)('blocks answer, cancel, and input callbacks while %s', (state) => {
    const onAnswer = vi.fn(); const onChange = vi.fn(); const onCancel = vi.fn();
    render(<QuestionCard request={{ ...question, state }} value={{ selectedIds: ['a'], customText: '' }} onChange={onChange} onAnswer={onAnswer} onCancel={onCancel} />);
    const radio = screen.getByRole('radio', { name: 'Keyboard' });
    expect(radio).toBeDisabled();
    fireEvent.click(radio);
    fireEvent.submit(screen.getByRole('form'));
    fireEvent.click(screen.getByRole('button', { name: 'Cancel question' }));
    expect(onAnswer).not.toHaveBeenCalled(); expect(onChange).not.toHaveBeenCalled(); expect(onCancel).not.toHaveBeenCalled();
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
    expect(screen.getByRole('radio', { name: 'Layout' })).not.toBeChecked();
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
});
