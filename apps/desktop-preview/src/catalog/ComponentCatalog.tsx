import { useState, type ReactNode } from 'react';
import { ArrowCounterClockwiseIcon } from '@phosphor-icons/react/dist/csr/ArrowCounterClockwise';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { AvaMark } from '../components/AvaMark';
import { Button, IconButton } from '../ui/Button';
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
const sections = ['Foundations', 'Turns', 'Tools', 'Permissions', 'Questions', 'Run notices', 'Composer', 'Changes'];
const statuses: readonly StatusValue[] = ['working', 'completed', 'approval', 'stopped', 'failed', 'unavailable'];

function CaseSelect({ label, value, cases, onChange }: { label: string; value: string; cases: readonly string[]; onChange: (value: string) => void }) {
  return <label className={styles.caseSelect}>{label}<select value={value} onChange={(event) => { if (cases.includes(event.currentTarget.value)) onChange(event.currentTarget.value); }}>{cases.map((name) => <option key={name} value={name}>{name}</option>)}</select></label>;
}

function Section({ index, description, children }: { index: number; description: string; children: ReactNode }) {
  return <section id={`section-${index}`} className={styles.section} aria-labelledby={`section-title-${index}`}>
    <header className={styles.sectionHeading}><span>{String(index + 1).padStart(2, '0')}</span><div><h2 id={`section-title-${index}`}>{sections[index]}</h2><p>{description}</p></div></header>
    {children}
  </section>;
}

function Foundations({ receipt }: { receipt: Receipt }) {
  const [open, setOpen] = useState(false);
  const reduced = useReducedMotion();
  return <Section index={0} description="Native controls. Neutral surfaces. Motion that never owns state.">
    <Surface aria-label="Foundation examples">
      <div className={styles.controls}><Button tone="primary" onClick={() => receipt('Primary button')}>Primary action</Button><Button disabled>Unavailable action</Button><IconButton label="Record foundation intent" onClick={() => receipt('Icon button')}><PlusIcon size={18} aria-hidden="true" /></IconButton></div>
      <div className={styles.statuses}>{statuses.map((status) => <Status key={status} status={status} />)}</div>
      <Disclosure id="catalog-foundation-disclosure" open={open} onOpenChange={setOpen} trigger="Motion and material contract" triggerClassName={styles.textButton}>
        <p className={styles.note}>Panel/disclosure: 200ms ease-out. Feedback: 180ms. Working: icon-only, 2.4s linear. Reduced motion: {reduced ? 'on' : 'off'} by system preference. Closing is logically immediate; retained paint is inert. This catalog is opaque.</p>
      </Disclosure>
    </Surface>
  </Section>;
}

function Turns() {
  const [scenario, setScenario] = useState('committed');
  return <Section index={1} description="Supplied text only. No timers, generated continuations, or private reasoning.">
    <CaseSelect label="Turn example" value={scenario} cases={Object.keys(examples.turns)} onChange={setScenario} />
    <div className={styles.turn}><MessageTurn turn={examples.turns[scenario]} mark={examples.turns[scenario].role === 'assistant' ? <AvaMark /> : undefined} /></div>
  </Section>;
}

function Tools() {
  const [scenario, setScenario] = useState('lifecycle');
  return <Section index={2} description="Supplied activity and inline file diffs. Read-only examples; no tools ran or files changed.">
    <CaseSelect label="Tool example" value={scenario} cases={['lifecycle', ...Object.keys(examples.toolChangeCases)]} onChange={setScenario} />
    <Surface><ToolActivity key={scenario} records={scenario === 'lifecycle' ? examples.tools : examples.toolChangeCases[scenario]} /></Surface>
  </Section>;
}

function Permissions({ receipt }: { receipt: Receipt }) {
  const [scenario, setScenario] = useState('once');
  const [version, setVersion] = useState(1);
  const state = ['pending', 'submitted', 'error', 'stale', 'closed'].includes(scenario) ? scenario as PermissionView['state'] : 'ready';
  const request: PermissionView = { ...examples.permission, version, state, sessionAllowed: scenario === 'session', decisions: scenario === 'session' ? ['allow', 'allow_session', 'deny'] : ['allow', 'deny'], error: 'Invented request failure. Choose another example to revise the UI version.' };
  return <Section index={3} description="No default grant. Session scope appears only when explicitly advertised.">
    <CaseSelect label="Permission example" value={scenario} cases={['once', 'session', 'pending', 'submitted', 'error', 'stale', 'closed']} onChange={(value) => { setScenario(value); setVersion((v) => v + 1); }} />
    <PermissionCard request={request} onDecide={() => receipt('Permission choice')} />
    <Button className={styles.revise} onClick={() => { setVersion((v) => v + 1); setScenario('once'); }}>Revise permission fixture</Button>
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
    custom: scenario === 'secret' ? { kind: 'secret', label: 'Secret example answer' } : scenario === 'custom' ? { kind: 'text', label: 'Custom example answer' } : { kind: 'none' },
    error: 'Invented question failure. Revise the fixture to try again.',
  };
  function revise(next: string) { setScenario(next); setVersion((v) => v + 1); setValue({ selectedIds: [], customText: '' }); }
  return <Section index={4} description="Native single/multiple choice, optional custom input, and masked secrets. Answers are never echoed here.">
    <CaseSelect label="Question example" value={scenario} cases={['single', 'multiple', 'custom', 'secret', 'pending', 'submitted', 'error', 'stale', 'closed']} onChange={revise} />
    <QuestionCard request={request} value={value} onChange={setValue} onAnswer={() => receipt('Question answer')} onCancel={() => receipt('Question cancellation')} />
    <Button className={styles.revise} onClick={() => revise(scenario)}>Revise question fixture</Button>
  </Section>;
}

function Runs({ receipt }: { receipt: Receipt }) {
  const [scenario, setScenario] = useState('error');
  return <Section index={5} description="Retry attempts are supplied, not scheduled. Cancellation requested is not cancellation completed.">
    <CaseSelect label="Run example" value={scenario} cases={Object.keys(examples.runs)} onChange={setScenario} />
    <RunNotice view={examples.runs[scenario]} onRetry={scenario === 'error' ? () => receipt('Retry request') : undefined} />
  </Section>;
}

function Composer({ receipt }: { receipt: Receipt }) {
  const [view, setView] = useState(examples.composer);
  return <Section index={6} description="Controlled draft, mode, model, and references. Send/Stop emit local intent; they do not change run state.">
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
  const [expanded, setExpanded] = useState<readonly string[]>(['layout']);
  const view: ChangeReviewState = scenario === 'ready' ? { state: 'ready', changes: examples.changes }
    : scenario === 'stale' ? { state: 'stale', changes: examples.changes, message: 'Last supplied excerpts · stale, not current file state.' }
      : scenario === 'loading' ? { state: 'loading' }
        : scenario === 'error' ? { state: 'error', message: 'Example excerpts could not be loaded.' } : { state: 'empty', message: 'No supplied changes.' };
  return <Section index={7} description="Read-only resource states. No Git, filesystem, apply/reject, or editor authority.">
    <CaseSelect label="Changes example" value={scenario} cases={['ready', 'loading', 'empty', 'error', 'stale']} onChange={setScenario} />
    <Surface><ChangeReviewView view={view} notice="Invented excerpts only. No files are accessed or changed." expandedIds={expanded} onExpandedChange={(id, open) => setExpanded(open ? [...new Set([...expanded, id])] : expanded.filter((entry) => entry !== id))} /></Surface>
  </Section>;
}

function Examples() {
  const [receipt, setReceipt] = useState('No local intent received.');
  function receive(kind: string) { setReceipt(`${kind}: local fixture intent only. No policy changed, resolver settled, or command executed. Answer values are not recorded.`); }
  return <><p className={styles.receipt} role="status" aria-live="polite">{receipt}</p><Foundations receipt={receive} /><Turns /><Tools /><Permissions receipt={receive} /><Questions receipt={receive} /><Runs receipt={receive} /><Composer receipt={receive} /><Changes /></>;
}

export default function ComponentCatalog() {
  const [epoch, setEpoch] = useState(0);
  return <main className={styles.catalog} data-material="opaque" aria-label="Frontend component catalog">
    <header className={styles.header}><div className={styles.identity}><AvaMark /><span>AVA / FRONTEND KIT</span></div><div className={styles.controls}><a href="?">Back to chat</a><Button onClick={() => setEpoch((value) => value + 1)}><ArrowCounterClockwiseIcon size={16} aria-hidden="true" />Reset examples</Button></div></header>
    <div className={styles.intro}><p className={styles.kicker}>Browser-only component reference</p><h1>Quiet interfaces.<br />Explicit boundaries.</h1><p>Invented, local examples for a reusable chat frontend. Nothing is connected, saved, granted, uploaded, or executed. Reset or refresh clears local values.</p></div>
    <nav className={styles.index} aria-label="Component sections">{sections.map((name, index) => <a key={name} href={`#section-${index}`}>{name}</a>)}</nav>
    <div className={styles.examples}><Examples key={epoch} /></div>
    <footer className={styles.footer}>Experimental UI contracts, not RPC schemas. Child transcripts, host integration, permissions authority, persistence, light/system themes, and native tools remain outside this kit.</footer>
  </main>;
}
