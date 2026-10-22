import { useState, type ReactNode } from 'react';
import { ArrowCounterClockwiseIcon } from '@phosphor-icons/react/dist/csr/ArrowCounterClockwise';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { AvaMark } from '../components/AvaMark';
import { BackLink, Button, IconButton } from '../ui/Button';
import { Surface } from '../ui/Surface';
import { Disclosure } from '../ui/Disclosure';
import { Status, type StatusValue } from '../ui/Status';
import { useReducedMotion } from '../ui/useReducedMotion';
import type { ChangeReviewState, PermissionView, QuestionAnswer, QuestionView } from '../chat/contracts';
import { MessageTurn } from '../chat/widgets/MessageTurn';
import { ToolActivity } from '../components/ToolActivity';
import { PermissionCard } from '../chat/widgets/PermissionCard';
import { QuestionCard } from '../chat/widgets/QuestionCard';
import { RunNotice } from '../chat/widgets/RunNotice';
import { ChatComposer } from '../chat/widgets/ChatComposer';
import { ChangeReviewView } from '../chat/widgets/ChangeReviewView';
import * as examples from './examples';
import styles from './ComponentCatalog.module.css';

type Receipt = (kind: string) => void;
const sections = [
  { id: 'foundations', title: 'Foundations' }, { id: 'turns', title: 'Turns' }, { id: 'tools', title: 'Tools' }, { id: 'permissions', title: 'Permissions' },
  { id: 'questions', title: 'Questions' }, { id: 'runs', title: 'Run notices' }, { id: 'composer', title: 'Composer' }, { id: 'changes', title: 'Changes' },
] as const;
type SectionId = (typeof sections)[number]['id'];
const statuses: readonly StatusValue[] = ['queued', 'working', 'completed', 'approval', 'stopped', 'failed', 'unavailable'];

function CaseSelect({ label, value, cases, onChange }: { label: string; value: string; cases: readonly string[]; onChange: (value: string) => void }) {
  return <label className={styles.caseSelect}>{label}<select value={value} onChange={(event) => { if (cases.includes(event.currentTarget.value)) onChange(event.currentTarget.value); }}>{cases.map((name) => <option key={name} value={name}>{name}</option>)}</select></label>;
}

function Section({ id, description, children }: { id: SectionId; description: string; children: ReactNode }) {
  return <section id={`catalog-${id}`} className={styles.section} aria-labelledby={`catalog-${id}-title`}>
    <header className={styles.sectionHeading}><h2 id={`catalog-${id}-title`}>{sections.find((section) => section.id === id)?.title}</h2><p>{description}</p></header>
    {children}
  </section>;
}

function Foundations({ receipt }: { receipt: Receipt }) {
  const [open, setOpen] = useState(false);
  const reduced = useReducedMotion();
  return <Section id="foundations" description="Buttons, status labels and disclosure. Color appears only in status, always with an icon and a word.">
    <Surface aria-label="Foundation examples">
      <div className={styles.controls}><Button tone="primary" onClick={() => receipt('Primary button')}>Primary action</Button><Button disabled>Unavailable action</Button><IconButton label="Add item" onClick={() => receipt('Icon button')}><PlusIcon size={18} aria-hidden="true" /></IconButton></div>
      <div className={styles.statuses}>{statuses.map((status) => <Status key={status} status={status} />)}</div>
      <Disclosure id="catalog-foundation-disclosure" open={open} onOpenChange={setOpen} trigger="Motion timing" triggerClassName={styles.textButton}>
        <p className={styles.note}>Panels and disclosures open in 200ms with an ease-out curve; button feedback takes 180ms. The Working icon turns once every 2.4s and stays still when reduced motion is on (currently {reduced ? 'on' : 'off'}).</p>
      </Disclosure>
    </Surface>
  </Section>;
}

function Turns() {
  const [scenario, setScenario] = useState('committed');
  return <Section id="turns" description="One message each. Stopped and failed replies keep the text that arrived.">
    <CaseSelect label="Turn example" value={scenario} cases={Object.keys(examples.turns)} onChange={setScenario} />
    <div className={styles.turn}><MessageTurn turn={examples.turns[scenario]} mark={examples.turns[scenario].role === 'assistant' ? <AvaMark /> : undefined} /></div>
  </Section>;
}

function Tools() {
  const [scenario, setScenario] = useState('lifecycle');
  return <Section id="tools" description="Tool activity rows with output and inline file diffs, closed until asked for.">
    <CaseSelect label="Tool example" value={scenario} cases={['lifecycle', ...Object.keys(examples.toolChangeCases)]} onChange={setScenario} />
    <Surface><ToolActivity key={scenario} records={scenario === 'lifecycle' ? examples.tools : examples.toolChangeCases[scenario]} /></Surface>
  </Section>;
}

function Permissions({ receipt }: { receipt: Receipt }) {
  const [scenario, setScenario] = useState('once');
  const [version, setVersion] = useState(1);
  const state = ['pending', 'submitted', 'error', 'stale', 'closed'].includes(scenario) ? scenario as PermissionView['state'] : 'ready';
  const request: PermissionView = { ...examples.permission, version, state, sessionAllowed: scenario === 'session', decisions: scenario === 'session' ? ['allow', 'allow_session', 'deny'] : ['allow', 'deny'], error: 'This request couldn’t be loaded. Ask AVA to try again.' };
  return <Section id="permissions" description="Nothing is allowed by default. “Allow for this session” appears only when AVA offers it.">
    <CaseSelect label="Permission example" value={scenario} cases={['once', 'session', 'pending', 'submitted', 'error', 'stale', 'closed']} onChange={(value) => { setScenario(value); setVersion((v) => v + 1); }} />
    <PermissionCard request={request} onDecide={() => receipt('Permission choice')} />
    <Button className={styles.revise} onClick={() => { setVersion((v) => v + 1); setScenario('once'); }}>New request</Button>
  </Section>;
}

function Questions({ receipt }: { receipt: Receipt }) {
  const [scenario, setScenario] = useState('single');
  const [version, setVersion] = useState(1);
  const [value, setValue] = useState<QuestionAnswer>({ selectedIds: [], customText: '' });
  const request: QuestionView = { ...examples.question, version,
    state: ['pending', 'submitted', 'error', 'stale', 'closed'].includes(scenario) ? scenario as QuestionView['state'] : 'ready',
    selection: scenario === 'multiple' ? 'multiple' : 'single', searchable: scenario === 'multiple',
    options: scenario === 'secret' ? [] : examples.question.options,
    custom: scenario === 'secret' ? { kind: 'secret', label: 'Access token' } : scenario === 'custom' ? { kind: 'text', label: 'Describe the focus' } : { kind: 'none' },
    error: 'This question couldn’t be loaded. Nothing was answered.',
  };
  function revise(next: string) { setScenario(next); setVersion((v) => v + 1); setValue({ selectedIds: [], customText: '' }); }
  return <Section id="questions" description="Single or multiple choice, an optional answer of your own, and hidden answers for secrets.">
    <CaseSelect label="Question example" value={scenario} cases={['single', 'multiple', 'custom', 'secret', 'pending', 'submitted', 'error', 'stale', 'closed']} onChange={revise} />
    <QuestionCard request={request} value={value} onChange={setValue} onAnswer={() => receipt('Question answer')} onCancel={() => receipt('Question cancellation')} />
    <Button className={styles.revise} onClick={() => revise(scenario)}>New question</Button>
  </Section>;
}

function Runs({ receipt }: { receipt: Receipt }) {
  const [scenario, setScenario] = useState('error');
  return <Section id="runs" description="Errors, retries and stops are separate states. A requested stop is not shown as stopped until it is.">
    <CaseSelect label="Run example" value={scenario} cases={Object.keys(examples.runs)} onChange={setScenario} />
    <RunNotice view={examples.runs[scenario]} onRetry={scenario === 'error' ? () => receipt('Retry') : undefined} />
  </Section>;
}

function Composer({ receipt }: { receipt: Receipt }) {
  const [view, setView] = useState(examples.composer);
  return <Section id="composer" description="Draft, mode, model and references in one bounded input.">
    <CaseSelect label="Composer example" value={view.runState} cases={['idle', 'running']} onChange={(state) => setView({ ...view, runState: state === 'running' ? 'running' : 'idle' })} />
    <div className={styles.composer}><ChatComposer view={view}
      onDraftChange={(draft) => setView({ ...view, draft })}
      onModelChange={(modelId) => setView({ ...view, modelId })}
      onModeChange={(modeId) => setView({ ...view, modeId })}
      onExpandedChange={(expanded) => setView({ ...view, expanded })}
      onAddReference={(id) => setView({ ...view, selectedContextIds: [...new Set([...view.selectedContextIds, id])] })}
      onRemoveReference={(id) => setView({ ...view, selectedContextIds: view.selectedContextIds.filter((entry) => entry !== id) })}
      onSend={() => receipt('Composer send')} onStop={() => receipt('Composer stop')}
    /></div>
  </Section>;
}

function Changes() {
  const [scenario, setScenario] = useState('ready');
  const view: ChangeReviewState = scenario === 'ready' ? { state: 'ready', changes: examples.changes }
    : scenario === 'stale' ? { state: 'stale', changes: examples.changes, message: 'These changes may be out of date.' }
      : scenario === 'loading' ? { state: 'loading' }
        : scenario === 'error' ? { state: 'error', message: 'Couldn’t load the changes.' } : { state: 'empty', message: 'No file changes yet.' };
  return <Section id="changes" description="Every file the chat changed, as a unified diff.">
    <CaseSelect label="Changes example" value={scenario} cases={['ready', 'loading', 'empty', 'error', 'stale']} onChange={setScenario} />
    <Surface><ChangeReviewView view={view} /></Surface>
  </Section>;
}

function Examples() {
  const [receipt, setReceipt] = useState('Nothing received yet.');
  function receive(kind: string) { setReceipt(`${kind} received. Answers aren’t recorded.`); }
  return <><p className={styles.receipt} role="status" aria-live="polite">{receipt}</p><Foundations receipt={receive} /><Turns /><Tools /><Permissions receipt={receive} /><Questions receipt={receive} /><Runs receipt={receive} /><Composer receipt={receive} /><Changes /></>;
}

export default function ComponentCatalog() {
  const [epoch, setEpoch] = useState(0);
  return <main className={styles.catalog} data-material="opaque" aria-label="Frontend component catalog">
    <header className={styles.header}><div className={styles.identity}><AvaMark /><span>AVA / Component catalog</span></div><div className={styles.controls}><BackLink href="?">Back to chat</BackLink><Button onClick={() => setEpoch((value) => value + 1)}><ArrowCounterClockwiseIcon size={16} aria-hidden="true" />Reset examples</Button></div></header>
    <div className={styles.intro}><h1>Chat components</h1><p>Every state of AVA’s conversation building blocks, side by side. These are local examples: nothing is connected, saved, granted or run, and Reset clears them.</p></div>
    <nav className={styles.index} aria-label="Component sections">{sections.map((section) => <a key={section.id} href={`#catalog-${section.id}`}>{section.title}</a>)}</nav>
    <div className={styles.examples}><Examples key={epoch} /></div>
  </main>;
}
