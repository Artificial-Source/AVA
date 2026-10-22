import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { MainScreensProps } from './MainScreens';
import type { ModelSelection } from './contracts';
import { AppShell } from '../components/AppShell';
import { DialogFrame } from '../ui/DialogFrame';
import { installDialogTestSupport } from './dialogTestSupport';

const host = vi.hoisted(() => {
  let resolve!: () => void;
  const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, props: null as MainScreensProps | null, loads: 0, switchParent: false };
});
// Hold only the lazy content. The actual main request guard and native loading
// modal run unchanged; no production loader or store test seams are added.
vi.mock('./MainScreens', async () => {
  host.loads++;
  await host.ready;
  return { default: (props: MainScreensProps) => {
    host.props = props;
    if (props.request.kind === 'settings') return <div><h1>Settings</h1><button onClick={props.onDismiss}>Back to chat</button></div>;
    return <DialogFrame title="Loaded test screen" onDismiss={props.onDismiss} returnFocus={props.returnFocus}><p>Loaded local content</p></DialogFrame>;
  } };
});
// Simulate an external owner replacing the viewed parent at the reducer boundary.
vi.mock('../state/reducer', async (original) => {
  const actual = await original<typeof import('../state/reducer')>();
  const { previewReducer } = actual;
  return { ...actual, previewReducer: ((state, action) => {
    if (host.switchParent && action.type === 'setModel') {
      host.switchParent = false;
      return previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    }
    return previewReducer(state, action);
  }) as typeof previewReducer };
});
beforeEach(() => { installDialogTestSupport(); host.props = null; host.switchParent = false; });
function modelButton() { return screen.getByRole('button', { name: 'Model for this chat' }); }
async function open() {
  modelButton().focus(); fireEvent.click(modelButton());
  await screen.findByRole('dialog', { name: 'Loaded test screen' });
  return host.props!;
}
function choice(props: MainScreensProps, patch: Partial<ModelSelection> = {}): ModelSelection {
  return { id: props.request.id, version: props.request.version, modelId: 'orion', scope: 'current-chat', reasoning: null, ...patch };
}

describe('latest main-screen scope and pending boundary', () => {
  it('defers screen content and makes pending cancellation, reopen and latest target safe without sending or retargeting the background', async () => {
    render(<AppShell />);
    expect(host.loads).toBe(0); expect(host.props).toBeNull();
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const projectChats = within(screen.getByRole('navigation', { name: 'All project chats' }));
    const exportChat = projectChats.getByRole('button', { name: 'Export date range Attention' });
    const keyboardChat = projectChats.getByRole('button', { name: 'Keyboard navigation Working' });
    const newChat = screen.getByRole('button', { name: 'New chat' }); const settings = screen.getByRole('button', { name: 'Settings' });
    const messages = screen.queryAllByRole('article', { name: 'Your message' }).length;
    const model = modelButton(); model.focus(); fireEvent.click(model);
    const pending = screen.getByRole('dialog', { name: 'Loading models' }); expect(pending).toHaveAttribute('open');
    const draft = screen.getByRole('textbox'); const original = (draft as HTMLTextAreaElement).value;
    fireEvent.click(screen.getByRole('button', { name: 'Send' }));
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true });
    fireEvent.change(draft, { target: { value: 'Blocked draft change' } });
    fireEvent.click(exportChat); fireEvent.click(newChat); fireEvent.click(settings);
    fireEvent.click(screen.getByRole('button', { name: /^Working Review back navigation/ }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.getByRole('textbox')).toHaveValue(original);
    expect(screen.queryAllByRole('article', { name: 'Your message' })).toHaveLength(messages);
    fireEvent.keyDown(pending, { key: 'Escape', isComposing: true }); expect(pending).toBeInTheDocument();
    fireEvent.keyDown(pending, { key: 'Escape' }); expect(pending).not.toBeInTheDocument(); expect(model).toHaveFocus();
    settings.focus(); fireEvent.click(settings);
    const page = screen.getByRole('region', { name: 'Settings page' });
    const pendingBack = within(page).getByRole('button', { name: 'Back to chat' });
    expect(page).toHaveFocus(); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(within(page).getByRole('status')).toHaveTextContent('Loading…');
    expect(sidebar).toBeInTheDocument(); expect(sidebar).not.toBeVisible();
    expect(sidebar).toHaveAttribute('hidden'); expect(sidebar).toHaveAttribute('inert');
    expect(screen.queryByRole('complementary', { name: 'Projects and chats' })).not.toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'All project chats' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'New chat' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Settings' })).not.toBeInTheDocument();
    // Retained controls have no authority even before the Settings body loads.
    fireEvent.change(draft, { target: { value: 'Hidden draft edit' } }); fireEvent.submit(draft.closest('form')!);
    fireEvent.click(exportChat); fireEvent.click(keyboardChat); fireEvent.click(newChat); fireEvent.click(settings);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(page); expect(draft).toHaveValue(original);
    fireEvent.click(pendingBack);
    expect(page).not.toBeInTheDocument(); expect(settings).toHaveFocus(); expect(settings).toBeVisible();
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar);
    expect(sidebar).not.toHaveAttribute('hidden'); expect(sidebar).not.toHaveAttribute('inert');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Keyboard navigation Working' }));
    const stop = screen.getByRole('button', { name: 'Stop' });
    fireEvent.click(settings); fireEvent.click(stop);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBeInTheDocument();
    await act(async () => { host.resolve(); await host.ready; });
    await screen.findByRole('button', { name: 'Back to chat' });
    expect(host.props?.request).toMatchObject({ kind: 'settings', version: 3, parentChatId: 'keyboard-navigation', inspectedChildId: null });
    expect(screen.getByRole('region', { name: 'Settings page' })).toBeInTheDocument();
    expect(sidebar).toHaveAttribute('hidden'); expect(sidebar).toHaveAttribute('inert');
    fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); expect(settings).toHaveFocus(); expect(sidebar).toBeVisible();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(modelButton()).toHaveTextContent('Aster Mini');
    expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
  });

  it('requires Back before sidebar navigation or New chat and rejects departed Settings callbacks', async () => {
    render(<AppShell />);
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const exportChat = within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Export date range Attention' });
    const newChat = screen.getByRole('button', { name: 'New chat' }); const settings = screen.getByRole('button', { name: 'Settings' });
    fireEvent.click(settings);
    await screen.findByRole('button', { name: 'Back to chat' }); const old = host.props!;
    fireEvent.click(exportChat); fireEvent.click(newChat);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBeInTheDocument(); expect(host.props?.request).toBe(old.request);
    expect(sidebar).toHaveAttribute('hidden'); expect(sidebar).toHaveAttribute('inert');
    expect(screen.queryByRole('navigation', { name: 'All project chats' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); expect(settings).toHaveFocus(); expect(sidebar).toBeVisible();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Export date range Attention' }));
    act(() => { old.onSelectModel(choice(old)); old.onPreferencesChange({ ...old.preferences, enter: 'newline' }); old.onSectionChange('about'); old.onDismiss(); });
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('button', { name: 'Back to chat' });
    const current = host.props!;
    act(() => { old.onSelectModel(choice(old, { modelId: 'aster-mini' })); old.onPreferencesChange({ ...old.preferences, enter: 'newline' }); old.onSectionChange('about'); old.onDismiss(); });
    expect(host.props?.request).toBe(current.request); expect(screen.getByRole('region', { name: 'Settings page' })).toBeInTheDocument();
    expect(host.props!.parent.model).toBe('orion'); expect(host.props!.preferences.enter).toBe('send'); expect(host.props!.section).toBe('general');
    act(() => current.onSelectModel(choice(current, { scope: 'new-chat-default', modelId: 'aster-mini' })));
    fireEvent.click(newChat); expect(host.props?.request).toBe(current.request);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); expect(settings).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'New chat' }));
    act(() => current.onSelectModel(choice(current, { scope: 'new-chat-default', modelId: 'orion' })));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    expect(modelButton()).toHaveTextContent('Aster Mini');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('button', { name: 'Back to chat' });
    expect(host.props!.defaultModel).toBe('aster-mini');
  });

  it('rejects unknown models, forged identity/version/scope and fake reasoning, and ignores closed or old generation callbacks', async () => {
    const result = render(<AppShell />); const first = await open();
    for (const patch of [{ modelId: 'unknown' }, { id: 'foreign' }, { version: 0 }, { scope: 'unknown' }, { reasoning: 'High' }]) {
      act(() => first.onSelectModel(choice(first, patch as Partial<ModelSelection>)));
      expect(modelButton()).toHaveTextContent(/^Aster$/);
    }
    act(() => first.onSelectModel(choice(first)));
    expect(modelButton()).toHaveTextContent('Orion');
    act(first.onDismiss); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    act(() => first.onSelectModel(choice(first, { modelId: 'aster-mini' }))); expect(modelButton()).toHaveTextContent('Orion');
    const second = await open(); expect(second.request.version).toBeGreaterThan(first.request.version);
    act(() => { first.onDismiss(); first.onPreferencesChange({ ...first.preferences, density: 'compact' }); first.onSelectModel(choice(first, { modelId: 'aster-mini' })); });
    expect(screen.getByRole('dialog')).toBeInTheDocument(); expect(modelButton()).toHaveTextContent('Orion');
    expect(host.props?.preferences.density).toBe('comfortable');
    act(() => second.onSelectModel(choice(second, { scope: 'new-chat-default', modelId: 'aster-mini' })));
    expect(host.props?.defaultModel).toBe('aster-mini'); expect(modelButton()).toHaveTextContent('Orion');
    result.unmount(); act(() => { second.onSelectModel(choice(second)); second.onDismiss(); });
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('invalidates a changed parent scope and never revives it on return to the same parent', async () => {
    render(<AppShell />); const first = await open(); host.switchParent = true;
    act(() => first.onSelectModel(choice(first)));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    act(() => { first.onSelectModel(choice(first, { modelId: 'aster-mini' })); first.onDismiss(); });
    expect(modelButton()).toHaveTextContent('Orion');
    fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Remember search filters' }));
    const second = await open();
    act(() => first.onSelectModel(choice(first, { modelId: 'aster-mini' })));
    expect(modelButton()).toHaveTextContent(/^Aster$/);
    expect(host.props?.request).toBe(second.request);
    act(second.onDismiss);
  });

  it('retains only current Settings-owned opaque snapshots, rejecting closed, old-generation, model-screen and unmounted writers', async () => {
    const app = render(<AppShell />);
    const openSettings = async () => { fireEvent.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('button', { name: 'Back to chat' }); return host.props!; };
    const first = await openSettings(); expect(first.workspaceSeed).toBeUndefined();
    const snapshot = { opaque: 'owned by lazy definitions' }; act(() => first.onWorkspaceSnapshot!(snapshot));
    act(first.onDismiss); act(() => first.onWorkspaceSnapshot!({ opaque: 'late' }));
    const second = await openSettings(); expect(second.workspaceSeed).toBe(snapshot);
    act(() => first.onWorkspaceSnapshot!({ opaque: 'old generation' })); act(second.onDismiss);
    const models = await open(); expect(models.workspaceSeed).toBe(snapshot);
    act(() => models.onWorkspaceSnapshot!({ opaque: 'wrong surface' })); act(models.onDismiss);
    const third = await openSettings(); expect(third.workspaceSeed).toBe(snapshot);
    const latestSnapshot = { opaque: 'latest definitions' }; act(() => third.onWorkspaceSnapshot!(latestSnapshot)); act(third.onDismiss);
    expect((await openSettings()).workspaceSeed).toBe(latestSnapshot);
    const last = host.props!; app.unmount(); act(() => last.onWorkspaceSnapshot!({ opaque: 'unmounted' }));
    render(<AppShell />); expect((await openSettings()).workspaceSeed).toBeUndefined();
  });

  it('guards current Settings-owned appearance/material writes against stale, duplicate, closed and model-screen callbacks', async () => {
    const app = render(<AppShell />);
    const openSettings = async () => { fireEvent.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('button', { name: 'Back to chat' }); return host.props!; };
    const first = await openSettings();
    act(() => { first.onOpaqueChange(true); first.onOpaqueChange(true); });
    expect(host.props!.opaque).toBe(true); expect(screen.getByRole('main')).toHaveAttribute('data-material', 'opaque');
    act(() => first.onOpaqueChange(false)); expect(host.props!.opaque).toBe(true);
    const current = host.props!;
    act(() => current.onOpaqueChange('glass' as unknown as boolean)); expect(host.props!.opaque).toBe(true);
    act(() => current.onPreferencesChange({ ...current.preferences, palette: 'midnight' }));
    act(() => current.onPreferencesChange({ ...current.preferences, palette: 'charcoal' }));
    expect(host.props!.preferences.palette).toBe('midnight');
    act(host.props!.onDismiss);
    act(() => { current.onOpaqueChange(false); current.onPreferencesChange(current.preferences); });
    expect(screen.getByRole('main')).toHaveAttribute('data-palette', 'midnight'); expect(screen.getByRole('main')).toHaveAttribute('data-material', 'opaque');
    const models = await open();
    act(() => { models.onOpaqueChange(false); models.onPreferencesChange({ ...models.preferences, palette: 'charcoal' }); });
    expect(host.props!.opaque).toBe(true); expect(host.props!.preferences.palette).toBe('midnight');
    act(models.onDismiss);
    const latest = await openSettings();
    act(() => { first.onOpaqueChange(false); current.onPreferencesChange(current.preferences); });
    expect(host.props!.opaque).toBe(true); expect(host.props!.preferences.palette).toBe('midnight');
    act(() => latest.onOpaqueChange(false)); expect(host.props!.opaque).toBe(false);
    app.unmount(); act(() => { latest.onOpaqueChange(true); latest.onPreferencesChange(latest.preferences); });
  });

  it('does not grant parent model authority while inspecting a child, but accepts a new-chat default', async () => {
    render(<AppShell />);
    fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await screen.findByRole('button', { name: 'Back to chat' }); const props = host.props!;
    expect(props.request.inspectedChildId).toBe('search-outline');
    act(() => props.onSelectModel(choice(props))); expect(host.props?.parent.model).toBe('aster');
    act(() => props.onSelectModel(choice(props, { scope: 'new-chat-default', modelId: 'aster-mini' })));
    expect(host.props?.defaultModel).toBe('aster-mini');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Settings');
    expect(screen.queryByRole('region', { name: /Child conversation:/ })).not.toBeInTheDocument();
    act(props.onDismiss); fireEvent.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(modelButton()).toHaveTextContent(/^Aster$/);
  });
});
