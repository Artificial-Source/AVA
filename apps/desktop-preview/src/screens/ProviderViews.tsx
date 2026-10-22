import { useId, useState } from 'react';
import { Disclosure } from '../ui/Disclosure';
import { ProviderMark } from '../ui/ProviderMark';
import { providerIdentities } from '../ui/providerIdentity';
import { providers as connectionExamples } from './screenExamples';
import { validIdentity } from '../chat/requestValidation';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { ConnectAction, ConnectIntent, ConnectMethod, ConnectView, Provider } from './contracts';
import { Confirmation, FormField, PreferenceRow } from './ScreenPrimitives';
import { useDialogTransitionFocus } from './useDialogTransitionFocus';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

const methods: Record<ConnectMethod, { label: string; description: string }> = {
  key: { label: 'Dummy API key', description: 'A masked practice field. Only the literal AVA-DEMO-KEY is accepted.' },
  browser: { label: 'Browser sign-in example', description: 'Read instructions and manually choose the next fixture. No browser is opened.' },
  device: { label: 'Device code example', description: 'Inspect a first-party demo code. No URL, clipboard or polling.' },
};
export function validProvider(provider: Provider) {
  const example = connectionExamples.find((entry) => entry.id === provider?.id);
  return Boolean(example && provider.label === example.label && provider.state === example.state && provider.source === example.source && typeof provider.id === 'string' && provider.id.trim() && typeof provider.label === 'string' && provider.label.trim() &&
    ['available', 'connected-example', 'external', 'error'].includes(provider.state) && ['none', 'stored-example', 'environment-example'].includes(provider.source) &&
    Array.isArray(provider.methods) && provider.methods.length === example.methods.length && provider.methods.every((method) => Object.hasOwn(methods, method) && example.methods.includes(method)) && new Set(provider.methods).size === provider.methods.length);
}
export function validConnectIntent(view: ConnectView, intent: ConnectIntent) {
  if (!validIdentity(view) || !validProvider(view.provider) || view.id !== intent.id || view.version !== intent.version) return false;
  const { provider, step, method } = view;
  if (provider.source === 'environment-example' || provider.state === 'external') return false;
  if (intent.action === 'disconnect') return step === 'disconnect' && provider.state === 'connected-example' && provider.source === 'stored-example';
  if (intent.action === 'back') return ['key', 'browser', 'device', 'waiting', 'error', 'canceled'].includes(step);
  if (intent.action === 'choose-method') return step === 'method' && provider.methods.includes(intent.method);
  if (intent.action === 'submit-demo-key') return step === 'key' && method === 'key' && provider.methods.includes('key');
  if (intent.action === 'show-waiting') return (step === 'browser' || step === 'device') && method === step && provider.methods.includes(step);
  if (intent.action === 'show-success' || intent.action === 'show-error') return step === 'waiting' && method !== null && provider.methods.includes(method);
  return intent.action === 'retry' && (step === 'error' || step === 'canceled') && method !== null && provider.methods.includes(method);
}

export function ProviderDirectory({ providers, onConnect, onDisconnect, active = true }: { providers: readonly Provider[]; onConnect: (id: string) => void; onDisconnect: (id: string) => void; active?: boolean }) {
  const [examplesOpen, setExamplesOpen] = useState(false);
  const examplesId = useId();
  const gate = useViewGuard(JSON.stringify([providers, active, examplesOpen]));
  const valid = providers.every(validProvider) && new Set(providers.map((provider) => provider.id)).size === providers.length;
  if (!valid) return <p className={styles.notice} role="alert">Unsupported provider directory. Connection controls are unavailable.</p>;
  return <div><p className={styles.muted}>Read-only brand catalog</p>
    <ul className={styles.providerGrid} aria-label="Provider brands">{Object.entries(providerIdentities).map(([id, identity]) => <li key={id} className={styles.providerCard}>
      <ProviderMark id={id} /><div><h3>{identity.label}</h3><span>{id}</span></div>
    </li>)}</ul>
    <div className={styles.connectionExamples}><Disclosure id={examplesId} open={examplesOpen} onOpenChange={(next) => { if (active && gate.valid()) setExamplesOpen(next); }} triggerClassName={styles.examplesTrigger} trigger={<><span>Connection examples</span><span aria-hidden="true">{examplesOpen ? '−' : '+'}</span></>}>
    <p className={styles.muted}>Four fictional providers · manual sign-in rehearsal</p>
    {providers.map((provider) => <PreferenceRow key={provider.id} title={provider.label} description={provider.description}>
      <div className={styles.field}><span className={styles.status} data-status={provider.state}>{provider.state === 'external' ? 'Externally managed' : provider.state === 'connected-example' ? 'Connected EXAMPLE' : provider.state === 'error' ? 'Example error' : 'Available example'}</span>
        {provider.source === 'stored-example' && provider.state === 'connected-example' ? <Button onClick={() => { if (active && examplesOpen && gate.valid()) onDisconnect(provider.id); }}>Disconnect example</Button> : <Button disabled={provider.state === 'external' || !provider.methods.length} title={provider.state === 'external' ? 'Environment credentials are managed outside this UI.' : undefined} onClick={() => {
          if (active && examplesOpen && gate.valid() && provider.state !== 'external' && provider.methods.length) onConnect(provider.id);
        }}>{provider.state === 'external' ? 'Managed externally' : provider.state === 'error' ? 'Review connection' : 'Connect example'}</Button>}
      </div>
    </PreferenceRow>)}
    </Disclosure></div>
  </div>;
}

function DummyKeyForm({ onSubmit }: { onSubmit: () => void }) {
  const [value, setValue] = useState('');
  const [error, setError] = useState('');
  return <form onSubmit={(event) => {
    event.preventDefault();
    if (value !== 'AVA-DEMO-KEY') { setValue(''); setError('Use only the literal AVA-DEMO-KEY. Other input was cleared.'); return; }
    setValue('');
    onSubmit();
  }}>
    <FormField label="Dummy API key" hint="Type AVA-DEMO-KEY. Never enter real credentials. This ephemeral field is not stored or included in intents." error={error}>{(props) => <input {...props} className={styles.input} type="password" autoComplete="off" spellCheck={false} maxLength={64} value={value} onChange={(event) => { setValue(event.currentTarget.value); setError(''); }} />}</FormField>
    <Button type="submit" tone="primary">Continue with dummy key</Button>
  </form>;
}

export function ProviderConnectDialog({ view, onIntent, ...chrome }: DialogChrome & { view: ConnectView; onIntent: (intent: ConnectIntent) => void }) {
  const gate = useViewGuard(JSON.stringify(view));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const providerValid = validIdentity(view) && validProvider(view.provider);
  const external = providerValid && (view.provider.source === 'environment-example' || view.provider.state === 'external');
  function emit(action: ConnectAction) {
    const intent = { ...action, id: view.id, version: view.version };
    if (validConnectIntent(view, intent) && gate.claim()) { focus.capture(); onIntent(intent); }
  }
  const canBack = providerValid && !external && ['key', 'browser', 'device', 'waiting', 'error', 'canceled'].includes(view.step);
  const methodValid = providerValid && view.method !== null && view.provider.methods.includes(view.method);
  const unsupported = !providerValid || !['method', 'key', 'browser', 'device', 'waiting', 'success', 'error', 'canceled', 'disconnect'].includes(view.step) ||
    (['key', 'browser', 'device'].includes(view.step) && (view.method !== view.step || !methodValid));
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title={!providerValid ? 'Connection unavailable' : view.step === 'disconnect' ? `Disconnect ${view.provider.label}?` : `Connect ${view.provider.label}`} description="Local sign-in rehearsal only. No credentials, browser launch, network or account creation." onBack={canBack ? () => emit({ action: 'back' }) : undefined}>
    <h2 className={`${styles.taskTitle} ${styles.providerHeading}`}>{providerValid ? <ProviderMark id={view.provider.id} /> : null}{!providerValid ? 'Connection unavailable' : view.step === 'disconnect' ? `Disconnect ${view.provider.label}?` : view.provider.label}</h2>
    <p className={styles.muted}>Local example only. No sign-in, browser launch or credential storage.</p>
    <div {...focus.content}>{unsupported ? <p role="alert" className={styles.notice}>Unsupported connection view. No actions can be submitted.</p> : external ? <div className={styles.notice}><p>Externally managed example. This UI cannot inspect or remove environment credentials.</p><Button disabled>Connect unavailable</Button></div> : <>
      {view.step === 'method' ? <><p className={styles.muted}>Choose a supplied connection method.</p><ul className={styles.list}>{view.provider.methods.map((method) => <li key={method}><button className={styles.result} onClick={() => emit({ action: 'choose-method', method })}><span><strong>{methods[method].label}</strong><small>{methods[method].description}</small></span><span aria-hidden="true">→</span></button></li>)}</ul>{view.provider.methods.length === 0 ? <p className={styles.notice}>No methods supplied. Connection unavailable.</p> : null}</> : null}
      {view.step === 'key' ? <DummyKeyForm key={`${view.id}:${view.version}`} onSubmit={() => emit({ action: 'submit-demo-key' })} /> : null}
      {view.step === 'browser' || view.step === 'device' ? <section>
        <h3>{view.step === 'browser' ? 'Browser sign-in instructions' : 'Device sign-in instructions'}</h3>
        <p className={styles.muted}>In a connected client, the host would open the provider’s sign-in page. Here, nothing opens and no request starts.</p>
        {view.step === 'device' ? <><p className={styles.muted}>AVA first-party DEMO code · not usable for authentication</p><code className={styles.code}>AVA-DEMO-2048</code></> : null}
        <Button onClick={() => emit({ action: 'show-waiting' })}>Show waiting example</Button>
      </section> : null}
      {view.step === 'waiting' ? <section><h3 className={styles.status} data-status="waiting">Waiting · supplied example</h3><p className={styles.muted}>Nothing is polling or verifying. Manually choose an outcome to inspect; it is not an authentication result.</p><div className={styles.inline}><Button disabled={!methodValid} onClick={() => emit({ action: 'show-success' })}>Show success example</Button><Button disabled={!methodValid} onClick={() => emit({ action: 'show-error' })}>Show error example</Button></div></section> : null}
      {view.step === 'success' ? <section><h3 className={styles.status} data-status="success">Connection success · EXAMPLE</h3><p className={styles.muted}>No credential was stored and no catalog was refreshed. A sign-in result and model availability are separate supplied states.</p><Button onClick={focus.dismiss}>Done</Button></section> : null}
      {view.step === 'error' || view.step === 'canceled' ? <section><h3 className={styles.status} data-status={view.step}>{view.step === 'error' ? 'Sign-in failed · example' : 'Sign-in canceled · example'}</h3><p className={styles.muted}>No credential was retained. Retry returns to this provider’s supplied method; it does not contact a service.</p><Button disabled={!methodValid} onClick={() => emit({ action: 'retry' })}>Retry example</Button></section> : null}
      {view.step === 'disconnect' ? <Confirmation label="Record disconnect intent" disabled={view.provider.source !== 'stored-example' || view.provider.state !== 'connected-example'} onConfirm={() => emit({ action: 'disconnect' })} onCancel={focus.dismiss}>
        <p className={styles.lead}>Only a stored example credential is in scope. Environment or externally managed credentials would remain untouched.</p><p className={styles.muted}>This records local intent, not deletion of credentials or a confirmed remote sign-out.</p>
      </Confirmation> : null}
    </>}
    {view.step !== 'disconnect' ? <div className={styles.actions}>{canBack ? <Button onClick={() => emit({ action: 'back' })}>Back</Button> : null}<Button onClick={focus.dismiss}>Cancel</Button></div> : null}</div>
  </DialogFrame>;
}
