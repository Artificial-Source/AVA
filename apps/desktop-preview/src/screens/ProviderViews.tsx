import { useEffect, useEffectEvent, useId, useRef, useState, type ReactNode } from 'react';
import { BrowserIcon } from '@phosphor-icons/react/dist/csr/Browser';
import { CaretRightIcon } from '@phosphor-icons/react/dist/csr/CaretRight';
import { CheckIcon } from '@phosphor-icons/react/dist/csr/Check';
import { ClockIcon } from '@phosphor-icons/react/dist/csr/Clock';
import { CopyIcon } from '@phosphor-icons/react/dist/csr/Copy';
import { DeviceMobileIcon } from '@phosphor-icons/react/dist/csr/DeviceMobile';
import { KeyIcon } from '@phosphor-icons/react/dist/csr/Key';
import { validIdentity } from '../chat/requestValidation';
import { ChatStatus } from '../components/ChatStatus';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import { ProviderMark } from '../ui/ProviderMark';
import type { StatusValue } from '../ui/Status';
import type { ConnectAction, ConnectIntent, ConnectMethod, ConnectStep, ConnectView, Provider } from './contracts';
import { FormField, ListNotice } from './ScreenPrimitives';
import { modelChoices, providers as knownProviders } from './screenExamples';
import { useDialogTransitionFocus } from './useDialogTransitionFocus';
import { useViewGuard } from './useViewGuard';
import screens from './screens.module.css';
import styles from './ProviderViews.module.css';

const methods: Record<ConnectMethod, { label: string; detail: (provider: string) => string; icon: ReactNode }> = {
  key: { label: 'API key', detail: (provider) => `Paste a key from your ${provider} account.`, icon: <KeyIcon size={18} aria-hidden="true" /> },
  browser: { label: 'Browser sign-in', detail: (provider) => `Sign in on ${provider}’s website.`, icon: <BrowserIcon size={18} aria-hidden="true" /> },
  device: { label: 'Sign in with a code', detail: () => 'Enter a short code on another device.', icon: <DeviceMobileIcon size={18} aria-hidden="true" /> },
};
const deviceCode = 'KQRT-4829';

// Sign-ins finish on the provider's side and a submitted key is checked, so these
// steps move on by themselves after a short wait. Back and Cancel stay available.
// Each step reads its delay when it starts, so tests can hold or shorten them.
export const connectDelays: Record<'browser' | 'device' | 'verify', number> = { browser: 3000, device: 5000, verify: 1200 };
const settles: Partial<Record<ConnectStep, Readonly<{ delay: keyof typeof connectDelays; action: 'show-waiting' | 'show-success' }>>> = {
  browser: { delay: 'browser', action: 'show-waiting' },
  device: { delay: 'device', action: 'show-waiting' },
  waiting: { delay: 'verify', action: 'show-success' },
};

// Identity, label and methods stay pinned to the known list. State may change for
// the visit, but only between coherent pairs: an environment-managed provider never
// becomes a stored credential and nothing else can claim to be environment-managed.
export function validProvider(provider: Provider) {
  const known = knownProviders.find((entry) => entry.id === provider?.id);
  if (!known || typeof provider.id !== 'string' || !provider.id.trim() || typeof provider.label !== 'string' || !provider.label.trim() || provider.label !== known.label) return false;
  const external = known.state === 'external' || known.source === 'environment-example';
  const coherent = external ? provider.state === 'external' && provider.source === 'environment-example'
    : provider.state === 'connected-example' ? provider.source === 'stored-example' : (provider.state === 'available' || provider.state === 'error') && provider.source === 'none';
  return coherent && Array.isArray(provider.methods) && provider.methods.length === known.methods.length && new Set(provider.methods).size === provider.methods.length &&
    provider.methods.every((method) => Object.hasOwn(methods, method) && known.methods.includes(method));
}
// Back walks toward the method list. A provider with one method never shows that list,
// so its method step is where the flow begins: only waiting has a step behind it, and
// leaving is Cancel or Escape, which return to wherever the flow was opened from.
function canGoBack({ provider, step }: ConnectView) {
  return step === 'waiting' || (provider.methods.length > 1 && ['key', 'browser', 'device', 'error', 'canceled'].includes(step));
}
// Where connecting begins: a failed provider on its error and cause, a provider with one
// sign-in method straight on that method, anything else on the method list.
export function connectStart(provider: Provider): Readonly<{ step: ConnectStep; method: ConnectMethod | null }> {
  const [only, ...others] = provider.methods;
  if (provider.state === 'error') return { step: 'error', method: only ?? null };
  return only && !others.length ? { step: only, method: only } : { step: 'method', method: null };
}
export function validConnectIntent(view: ConnectView, intent: ConnectIntent) {
  if (!validIdentity(view) || !validProvider(view.provider) || view.id !== intent.id || view.version !== intent.version) return false;
  const { provider, step, method } = view;
  if (provider.source === 'environment-example' || provider.state === 'external') return false;
  if (intent.action === 'disconnect') return step === 'disconnect' && provider.state === 'connected-example' && provider.source === 'stored-example';
  if (intent.action === 'back') return canGoBack(view);
  if (intent.action === 'choose-method') return step === 'method' && provider.methods.includes(intent.method);
  if (intent.action === 'choose-model') return step === 'success';
  if (intent.action === 'submit-demo-key') return step === 'key' && method === 'key' && provider.methods.includes('key');
  if (intent.action === 'show-waiting') return (step === 'browser' || step === 'device') && method === step && provider.methods.includes(step);
  if (intent.action === 'show-success' || intent.action === 'show-error') return step === 'waiting' && method !== null && provider.methods.includes(method);
  return intent.action === 'retry' && (step === 'error' || step === 'canceled') && method !== null && provider.methods.includes(method);
}

function providerStatus(provider: Provider): Readonly<{ status: StatusValue; label: string }> {
  if (provider.state === 'external') return { status: 'completed', label: 'Managed externally' };
  if (provider.state === 'connected-example') return { status: 'completed', label: 'Connected' };
  if (provider.state === 'error') return { status: 'failed', label: 'Error' };
  return { status: 'unavailable', label: 'Not connected' };
}
// A failed provider always says why; its description carries the cause.
function failureCause(provider: Provider) {
  return provider.state === 'error' && typeof provider.description === 'string' ? provider.description.trim() : '';
}
const connectable = (provider: Provider) => (provider.state === 'available' || provider.state === 'error') && provider.methods.length > 0;

// The search comes first, so a dialog opening on this list lands there, never on an
// action. It matches word by word against name and ID, like the finder (“open ai”
// finds OpenAI). Escape clears it, then goes on to close or leave.
export function ProviderDirectory({ providers, onConnect, onDisconnect, active = true }: { providers: readonly Provider[]; onConnect: (id: string) => void; onDisconnect: (id: string) => void; active?: boolean }) {
  const gate = useViewGuard(JSON.stringify([providers, active]));
  const causeId = useId();
  const [query, setQuery] = useState('');
  const composing = useRef(false);
  const valid = Array.isArray(providers) && providers.every(validProvider) && new Set(providers.map((provider) => provider.id)).size === providers.length;
  if (!valid) return <ListNotice state="error" message="Providers can’t be shown right now, so connections are turned off." />;
  if (!providers.length) return <ListNotice state="empty" message="No providers to show." />;
  const terms = query.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean);
  const shown = providers.filter((provider) => terms.every((term) => `${provider.label} ${provider.id}`.toLocaleLowerCase().includes(term)));
  const groups = [
    { title: 'Connected', rows: shown.filter((provider) => provider.state === 'connected-example' || provider.state === 'external') },
    { title: 'Available', rows: shown.filter((provider) => provider.state === 'available' || provider.state === 'error') },
  ];
  return <div className={styles.directory}>
    <FormField label="Search providers">{(props) => <input {...props} className={screens.input} type="search" autoComplete="off" spellCheck={false} value={query}
      onChange={(event) => setQuery(event.currentTarget.value)}
      onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}
      onKeyDown={(event) => {
        if (event.key !== 'Escape' || !query || composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
        event.preventDefault(); event.stopPropagation(); setQuery('');
      }} />}</FormField>
    {shown.length ? groups.map(({ title, rows }) => rows.length ? <div key={title}>
      <h4 className={styles.groupTitle}>{title}</h4>
      <ul className={styles.list} aria-label={`${title} providers`}>{rows.map((provider) => {
        const { status, label } = providerStatus(provider);
        const cause = failureCause(provider);
        const stored = provider.state === 'connected-example' && provider.source === 'stored-example';
        return <li key={provider.id} className={styles.row}>
          <ProviderMark id={provider.id} label={provider.label} />
          <span className={styles.identity}><span className={styles.name}>{provider.label}</span><span className={styles.id}>{provider.id}</span></span>
          <span className={styles.status}><ChatStatus status={status} label={label} />{cause ? <span id={`${causeId}-${provider.id}`} className={styles.cause}>{cause}</span> : null}</span>
          {/* data-provider-action lets a host return focus to this row when its button changes. */}
          <span className={styles.action}>
            {stored ? <Button data-provider-action={provider.id} aria-label={`Disconnect ${provider.label}`} onClick={() => { if (active && gate.valid()) onDisconnect(provider.id); }}>Disconnect</Button> : null}
            {connectable(provider) ? <Button data-provider-action={provider.id} aria-label={`Connect ${provider.label}`} aria-describedby={cause ? `${causeId}-${provider.id}` : undefined}
              onClick={() => { if (active && gate.valid()) onConnect(provider.id); }}>Connect</Button> : null}
          </span>
        </li>;
      })}</ul>
    </div> : null) : <ListNotice state="empty" message={`No providers match “${query.trim()}”.`} />}
  </div>;
}

function stepStatus(view: ConnectView, external: boolean): Readonly<{ status: StatusValue; label: string }> {
  if (external) return providerStatus(view.provider);
  if (view.step === 'browser' || view.step === 'device') return { status: 'approval', label: 'Waiting for you' };
  if (view.step === 'waiting') return { status: 'working', label: view.method === 'key' ? 'Verifying key…' : 'Confirming sign-in…' };
  if (view.step === 'success') return { status: 'completed', label: 'Connected' };
  if (view.step === 'error') return { status: 'failed', label: view.method === 'key' ? 'Key not accepted' : 'Sign-in failed' };
  if (view.step === 'canceled') return { status: 'stopped', label: 'Sign-in canceled' };
  return providerStatus(view.provider);
}

// One bar per step: Back on the left, Cancel and the primary action on the right.
function ActionBar({ onBack, children }: { onBack?: () => void; children: ReactNode }) {
  return <div className={styles.bar}>{onBack ? <Button onClick={onBack}>Back</Button> : null}<span className={styles.barEnd}>{children}</span></div>;
}

// Format only: whether the text could be a key at all. Anything else is refused here,
// before it leaves the field, with the reason named.
const minimumKeyLength = 20;
function keyProblem(key: string, provider: string) {
  if (!key) return 'Enter an API key.';
  if (/\s/.test(key)) return 'API keys don’t contain spaces, so this one was cleared for safety. Paste the key again.';
  if (key.length < minimumKeyLength) return `That’s too short to be an API key, so it was cleared for safety. Paste the whole key from your ${provider} account.`;
  return '';
}

// The key lives only in this field: it is cleared on every submit, accepted or not
// (nobody can fix a key behind the dots), and is never part of an intent. Its submit
// button sits in the dialog footer and joins this form through `id`.
function KeyForm({ id, provider, onSubmit }: { id: string; provider: string; onSubmit: () => void }) {
  const [value, setValue] = useState('');
  const [error, setError] = useState('');
  const guideId = useId();
  return <form id={id} className={styles.step} onSubmit={(event) => {
    event.preventDefault();
    const problem = keyProblem(value.trim(), provider);
    setValue('');
    setError(problem);
    if (!problem) onSubmit();
  }}>
    {/* Where a key comes from, without naming pages AVA can't vouch for. */}
    <p id={guideId} className={styles.lead}>Create a key in your {provider} account’s API settings. The key needs access to the models you want to use, and usage is billed by {provider}.</p>
    <FormField label="API key" hint={`AVA checks the key with ${provider} before saving it.`} error={error}>{(props) => <input {...props} aria-describedby={`${props['aria-describedby']} ${guideId}`} className={screens.input} type="password" autoComplete="off" spellCheck={false} maxLength={256} value={value} onChange={(event) => { setValue(event.currentTarget.value); setError(''); }} />}</FormField>
  </form>;
}

function DeviceCode() {
  const [copy, setCopy] = useState<'idle' | 'copied' | 'failed'>('idle');
  useEffect(() => {
    if (copy !== 'copied') return;
    const timer = window.setTimeout(() => setCopy('idle'), 2000);
    return () => window.clearTimeout(timer);
  }, [copy]);
  return <>
    <div className={styles.code}><code>{deviceCode}</code>
      <Button onClick={() => {
        const clipboard = navigator.clipboard;
        if (!clipboard?.writeText) { setCopy('failed'); return; }
        clipboard.writeText(deviceCode).then(() => setCopy('copied'), () => setCopy('failed'));
      }}>{copy === 'copied' ? <CheckIcon size={14} aria-hidden="true" /> : <CopyIcon size={14} aria-hidden="true" />}{copy === 'copied' ? 'Copied' : 'Copy code'}</Button>
      <span className="srOnly" role="status">{copy === 'copied' ? 'Code copied.' : ''}</span>
    </div>
    {copy === 'failed' ? <p className={styles.note} role="alert">Couldn’t copy. Select the code and copy it instead.</p> : null}
    <p className={styles.expiry}><ClockIcon size={14} aria-hidden="true" />The code expires in 15 minutes.</p>
  </>;
}

// `autoAdvance` off holds each step still for hosts that drive outcomes themselves.
export function ProviderConnectDialog({ view, onIntent, autoAdvance = true, ...chrome }: DialogChrome & { view: ConnectView; onIntent: (intent: ConnectIntent) => void; autoAdvance?: boolean }) {
  const viewKey = JSON.stringify(view);
  const gate = useViewGuard(viewKey);
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const keyForm = useId();
  const providerValid = validIdentity(view) && validProvider(view.provider);
  const { provider } = view;
  const external = providerValid && (provider.source === 'environment-example' || provider.state === 'external');
  function emit(action: ConnectAction) {
    const intent = { ...action, id: view.id, version: view.version };
    if (validConnectIntent(view, intent) && gate.claim()) { focus.capture(); onIntent(intent); }
  }
  const canBack = providerValid && !external && canGoBack(view);
  const back = canBack ? () => emit({ action: 'back' }) : undefined;
  const methodValid = providerValid && view.method !== null && provider.methods.includes(view.method);
  const unsupported = !providerValid || !['method', 'key', 'browser', 'device', 'waiting', 'success', 'error', 'canceled', 'disconnect'].includes(view.step) ||
    (['key', 'browser', 'device'].includes(view.step) && (view.method !== view.step || !methodValid));
  const label = providerValid ? provider.label : '';
  const cause = providerValid ? failureCause(provider) : '';
  const providerModels = providerValid ? modelChoices.filter((choice) => choice.providerId === provider.id) : [];
  const families = [...new Set(providerModels.map((choice) => choice.family?.label ?? choice.label))];
  const subtitles: Partial<Record<ConnectStep, string>> = {
    method: 'Choose how to sign in.',
    key: `Use a key from your ${label} account.`,
    browser: 'Finish signing in on the provider’s website.',
    device: 'Sign in from another device with a one-time code.',
  };

  // Each version settles at most once: emit claims the view, and a new step clears the timer.
  const pending = autoAdvance && !unsupported && !external ? settles[view.step] : undefined;
  const settle = useEffectEvent((action: ConnectAction) => emit(action));
  useEffect(() => {
    if (!pending) return;
    const timer = window.setTimeout(() => settle({ action: pending.action }), connectDelays[pending.delay]);
    return () => window.clearTimeout(timer);
  }, [viewKey, pending]);

  // While a step waits on the provider, Cancel is the one useful action, so it takes focus.
  const cancel = (focused = false) => <Button data-autofocus={focused || undefined} onClick={focus.dismiss}>Cancel</Button>;
  const done = <ActionBar><Button onClick={focus.dismiss}>Done</Button></ActionBar>;
  function step(): Readonly<{ content: ReactNode; actions: ReactNode }> {
    if (unsupported) return { content: <ListNotice state="error" message="This connection can’t be shown, so nothing can be submitted." />, actions: done };
    if (external) return { content: <p className={styles.lead}>{label} uses credentials from your environment. Change or remove them there; AVA can’t edit them.</p>, actions: done };
    switch (view.step) {
      case 'method': return {
        content: <>
          {provider.state === 'error' ? <p className={styles.lead}>The last connection to {label} failed.{cause ? ` ${cause}` : ''} Connect again to restore it.</p> : null}
          {provider.methods.length ? <ul className={styles.methods} aria-label="Sign-in methods">{provider.methods.map((method) => <li key={method}>
            <button type="button" className={styles.method} onClick={() => emit({ action: 'choose-method', method })}>
              <span className={styles.methodIcon}>{methods[method].icon}</span>
              <span><strong>{methods[method].label}</strong><small>{methods[method].detail(label)}</small></span>
              <CaretRightIcon size={14} aria-hidden="true" />
            </button>
          </li>)}</ul> : <ListNotice state="unavailable" message={`${label} has no sign-in methods available.`} />}
        </>,
        actions: <ActionBar>{cancel()}</ActionBar>,
      };
      case 'key': return {
        content: <KeyForm key={`${view.id}:${view.version}`} id={keyForm} provider={label} onSubmit={() => emit({ action: 'submit-demo-key' })} />,
        actions: <ActionBar onBack={back}>{cancel()}<Button type="submit" form={keyForm} tone="primary">Verify key</Button></ActionBar>,
      };
      case 'browser': return {
        content: <p className={styles.lead}>Finish signing in to {label} in your browser. AVA continues on its own when you’re done.</p>,
        actions: <ActionBar onBack={back}>{cancel(true)}</ActionBar>,
      };
      case 'device': return {
        content: <>
          <p className={styles.lead}>On another device, open {label}’s sign-in page and enter this code. AVA continues on its own once it’s entered.</p>
          <DeviceCode />
        </>,
        actions: <ActionBar onBack={back}>{cancel()}</ActionBar>,
      };
      case 'waiting': return {
        content: <p className={styles.lead}>{view.method === 'key' ? `Checking the key with ${label}.` : `Confirming your sign-in with ${label}.`}</p>,
        actions: <ActionBar onBack={back}>{cancel(true)}</ActionBar>,
      };
      case 'success': return {
        content: <p className={styles.lead}>{label} is connected.{families.length ? ` ${families.join(' and ')} models are ready to use.` : ''}</p>,
        actions: <ActionBar>{providerModels.length ? <><Button onClick={focus.dismiss}>Done</Button><Button tone="primary" data-autofocus onClick={() => emit({ action: 'choose-model' })}>Choose a model</Button></>
          : <Button tone="primary" data-autofocus onClick={focus.dismiss}>Done</Button>}</ActionBar>,
      };
      case 'error': return {
        content: <p className={styles.lead}>{cause ? `${label} couldn’t connect. ${cause} ${view.method === 'key' ? 'Enter a new key to reconnect.' : 'Sign in again to reconnect.'}`
          : view.method === 'key' ? `${label} didn’t accept the API key. Check the key and try again.` : `Signing in to ${label} didn’t finish. Try again.`}</p>,
        actions: <ActionBar onBack={back}>{cancel()}<Button tone="primary" data-autofocus disabled={!methodValid} onClick={() => emit({ action: 'retry' })}>Try again</Button></ActionBar>,
      };
      case 'canceled': return {
        content: <p className={styles.lead}>Sign-in was canceled before it finished. Nothing changed.</p>,
        actions: <ActionBar onBack={back}>{cancel()}<Button tone="primary" data-autofocus disabled={!methodValid} onClick={() => emit({ action: 'retry' })}>Try again</Button></ActionBar>,
      };
      case 'disconnect': return {
        content: <>
          <p className={styles.lead}>{families.length ? `${families.join(' and ')} models` : `Models from ${label}`} will be unavailable for new chats.</p>
          <p className={styles.note}>You can connect {label} again at any time.</p>
        </>,
        actions: <ActionBar>{cancel()}<Button tone="destructive" disabled={provider.source !== 'stored-example' || provider.state !== 'connected-example'} onClick={() => emit({ action: 'disconnect' })}>Disconnect</Button></ActionBar>,
      };
    }
  }

  const { content, actions } = step();
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} onBack={back} contentProps={focus.content} footer={actions}
    title={!providerValid ? 'Connection unavailable' : external ? label : view.step === 'disconnect' ? `Disconnect ${label}?` : `Connect ${label}`}
    subtitle={providerValid && !external && !unsupported ? subtitles[view.step] : undefined}>
    <div className={styles.flow}>
      {providerValid ? <div className={styles.providerLine}><ProviderMark id={provider.id} label={provider.label} /><code>{provider.id}</code>
        <span aria-live="polite" aria-atomic="true"><ChatStatus {...stepStatus(view, external)} /></span></div> : null}
      {content}
    </div>
  </DialogFrame>;
}
