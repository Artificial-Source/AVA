import { type ChangeEvent, type ReactElement, useState } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, expectTypeOf, it, vi } from 'vitest';
import { AppearanceSettings } from './AppearanceSettings';
import { defaultAppearance } from './behaviorPreferences';
import type { Preferences } from './contracts';

const captured = vi.hoisted(() => ({ controls: new Map<string, (event: ChangeEvent<HTMLSelectElement>) => void>(), reset: () => {} }));
vi.mock('./ScreenPrimitives', async (original) => {
  const primitives = await original<typeof import('./ScreenPrimitives')>();
  return { ...primitives, PreferenceRow: (props: Parameters<typeof primitives.PreferenceRow>[0]) => {
    const control = props.children as ReactElement<{ 'aria-label': string; onChange: (event: ChangeEvent<HTMLSelectElement>) => void }>;
    if (control?.props.onChange) captured.controls.set(control.props['aria-label'], control.props.onChange);
    return <primitives.PreferenceRow {...props} />;
  } };
});
vi.mock('../ui/Button', async (original) => {
  const { Button } = await original<typeof import('../ui/Button')>();
  return { Button: (props: Parameters<typeof Button>[0]) => {
    if (props.children === 'Reset appearance') captured.reset = props.onClick as () => void;
    return <Button {...props} />;
  } };
});
const base: Preferences = { density: 'comfortable', motion: 'system', help: false, enter: 'newline', notifications: 'attention' };
const changes = [
  ['Interface font', 'uiFont', 'system'], ['Conversation text size', 'proseSize', 'large'],
  ['Conversation line spacing', 'proseSpacing', 'relaxed'], ['Reading width', 'readingWidth', 'narrow'],
  ['Contrast', 'contrast', 'higher'], ['Settings-row density', 'density', 'compact'], ['App motion', 'motion', 'reduced'],
] as const;

function Host() {
  const [preferences, setPreferences] = useState(base); const [opaque, setOpaque] = useState(false);
  return <AppearanceSettings preferences={preferences} opaque={opaque} active onChange={setPreferences} onOpaqueChange={setOpaque} onReset={() => { setPreferences({ ...preferences, ...defaultAppearance }); setOpaque(false); }} />;
}

describe('bounded local appearance controls', () => {
  it('accepts absent optional metadata as the unchanged visual defaults, with native accessible controls and a concise sample', () => {
    render(<Host />);
    expect(screen.getByRole('radio', { name: 'Default' })).toBeChecked();
    expect(screen.getAllByRole('radio')).toHaveLength(3);
    for (const [label, key] of changes) expect(screen.getByRole('combobox', { name: label })).toHaveValue(defaultAppearance[key]);
    expect(screen.getByRole('combobox', { name: 'Sidebar material' })).toHaveValue('glass');
    expect(screen.getByRole('figure', { name: 'Live appearance preview' })).toHaveTextContent('A little room to think.');
    expect(screen.getByRole('button', { name: 'Reset appearance' })).toBeDisabled();
    expect(screen.getByRole('radiogroup', { name: 'Dark palette' })).toHaveAccessibleDescription('AVA is dark only. Each palette is a neutral shade.');
    expectTypeOf<Pick<Preferences, 'palette'>>().toEqualTypeOf<Readonly<{ palette?: 'default' | 'charcoal' | 'midnight' }>>();
    expectTypeOf<Pick<Preferences, 'readingWidth'>>().toEqualTypeOf<Readonly<{ readingWidth?: 'narrow' | 'balanced' | 'wide' }>>();
  });
  it.each(changes)('validates %s and preserves all unrelated preferences', async (label, key, value) => {
    const onChange = vi.fn(); const user = userEvent.setup();
    render(<AppearanceSettings preferences={base} opaque={false} active onChange={onChange} onOpaqueChange={vi.fn()} onReset={vi.fn()} />);
    const select = screen.getByRole('combobox', { name: label });
    const change = captured.controls.get(label)!;
    for (const invalid of ['', 'unknown', '7800', 'url(https://example.com/font)']) act(() => change({ currentTarget: { value: invalid } } as ChangeEvent<HTMLSelectElement>));
    expect(onChange).not.toHaveBeenCalled();
    await user.selectOptions(select, value);
    expect(onChange).toHaveBeenCalledExactlyOnceWith({ ...base, [key]: value });
    act(() => change({ currentTarget: { value } } as ChangeEvent<HTMLSelectElement>));
    expect(onChange).toHaveBeenCalledTimes(1);
  });
  it('rejects forged palette values without consuming the next genuine choice, and supports radio keyboard selection', async () => {
    const user = userEvent.setup(); const onChange = vi.fn();
    const view = render(<AppearanceSettings preferences={base} opaque={false} active onChange={onChange} onOpaqueChange={vi.fn()} onReset={vi.fn()} />);
    const radio = screen.getByRole('radio', { name: 'Charcoal' }) as HTMLInputElement;
    radio.value = 'light'; fireEvent.click(radio); expect(onChange).not.toHaveBeenCalled();
    radio.value = 'charcoal'; fireEvent.click(radio); expect(onChange).toHaveBeenCalledExactlyOnceWith({ ...base, palette: 'charcoal' });
    view.unmount(); render(<Host />);
    screen.getByRole('radio', { name: 'Default' }).focus(); await user.keyboard('{ArrowRight}');
    expect(screen.getByRole('radio', { name: 'Charcoal' })).toBeChecked();
    await user.keyboard('{ArrowRight}'); expect(screen.getByRole('radio', { name: 'Midnight' })).toBeChecked();
  });
  it('applies each material intent once, validates its enum and rejects old, hidden and unmounted control/reset closures', () => {
    const onChange = vi.fn(); const onOpaqueChange = vi.fn(); const onReset = vi.fn();
    const props = { preferences: { ...base, palette: 'charcoal' as const }, opaque: false, active: true, onChange, onOpaqueChange, onReset };
    const view = render(<AppearanceSettings {...props} />);
    const material = captured.controls.get('Sidebar material')!; const font = captured.controls.get('Interface font')!; const reset = captured.reset;
    const intent = (value: string) => ({ currentTarget: { value } } as ChangeEvent<HTMLSelectElement>);
    act(() => { material(intent('unknown')); material(intent('glass')); }); expect(onOpaqueChange).not.toHaveBeenCalled();
    act(() => { material(intent('opaque')); material(intent('opaque')); }); expect(onOpaqueChange).toHaveBeenCalledExactlyOnceWith(true);
    view.rerender(<AppearanceSettings {...props} opaque />);
    act(() => { material(intent('opaque')); font(intent('system')); reset(); });
    expect(onChange).not.toHaveBeenCalled(); expect(onReset).not.toHaveBeenCalled(); expect(onOpaqueChange).toHaveBeenCalledTimes(1);
    const current = captured.controls.get('Interface font')!; const currentReset = captured.reset;
    view.rerender(<AppearanceSettings {...props} opaque active={false} />);
    act(() => { current(intent('system')); currentReset(); captured.reset(); captured.controls.get('Sidebar material')!(intent('glass')); });
    expect(onChange).not.toHaveBeenCalled(); expect(onReset).not.toHaveBeenCalled(); expect(onOpaqueChange).toHaveBeenCalledTimes(1);
    view.rerender(<AppearanceSettings {...props} opaque />);
    const closed = captured.controls.get('Interface font')!; const closedReset = captured.reset;
    view.unmount(); act(() => { closed(intent('system')); closedReset(); });
    expect(onChange).not.toHaveBeenCalled(); expect(onReset).not.toHaveBeenCalled();
  });
  it('resets only visual values, claims reset once and respects the standalone opaque baseline', async () => {
    const user = userEvent.setup(); const onReset = vi.fn(); const onChange = vi.fn();
    const view = render(<AppearanceSettings preferences={base} opaque active resetOpaque onChange={onChange} onOpaqueChange={vi.fn()} onReset={onReset} />);
    expect(screen.getByRole('button', { name: 'Reset appearance' })).toBeDisabled();
    view.rerender(<AppearanceSettings preferences={base} opaque={false} active resetOpaque onChange={onChange} onOpaqueChange={vi.fn()} onReset={onReset} />);
    const reset = captured.reset; act(() => { reset(); reset(); }); expect(onReset).toHaveBeenCalledTimes(1); expect(onChange).not.toHaveBeenCalled();
    view.unmount(); render(<Host />);
    await user.click(screen.getByRole('radio', { name: 'Midnight' }));
    for (const [label, , value] of changes) await user.selectOptions(screen.getByRole('combobox', { name: label }), value);
    await user.selectOptions(screen.getByLabelText('Sidebar material'), 'opaque');
    await user.click(screen.getByRole('button', { name: 'Reset appearance' }));
    expect(screen.getByRole('radio', { name: 'Default' })).toBeChecked();
    for (const [label, key] of changes) expect(screen.getByLabelText(label)).toHaveValue(defaultAppearance[key]);
    expect(screen.getByLabelText('Sidebar material')).toHaveValue('glass');
  });
});
