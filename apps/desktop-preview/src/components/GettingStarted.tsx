import { useId, useState } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { CheckCircleIcon } from '@phosphor-icons/react/dist/csr/CheckCircle';
import { CircleIcon } from '@phosphor-icons/react/dist/csr/Circle';
import { Button } from '../ui/Button';
import styles from './GettingStarted.module.css';

export type GettingStartedState = { providerConnected: boolean; projectOpened: boolean; firstMessageSent: boolean };

type Step = Readonly<{ id: string; done: boolean; title: string; detail: string; action?: Readonly<{ label: string; name: string; run: () => void }> }>;

// The three things a new user does before AVA is useful, in order. Each step is done by doing the
// real thing elsewhere in the app; the actions open those places, never a separate tutorial.
// `collapsible`: once every step is done, the checklist folds to one line (“You’re set up.”)
// with Show steps, so a set-up user's eye lands on what follows. The Welcome never folds it.
export function GettingStarted({ state, onConnectProvider, onOpenProject, headingLevel = 2, collapsible = false }: {
  state: GettingStartedState;
  onConnectProvider: () => void;
  onOpenProject: () => void;
  headingLevel?: 2 | 3;
  collapsible?: boolean;
}) {
  const id = useId();
  const [expanded, setExpanded] = useState(false);
  const Heading = headingLevel === 3 ? 'h3' : 'h2';
  const steps: readonly Step[] = [
    { id: 'provider', done: state.providerConnected, title: 'Connect a provider', detail: 'AVA replies using models from a provider you sign in to.', action: { label: 'Connect', name: 'Connect a provider', run: onConnectProvider } },
    { id: 'project', done: state.projectOpened, title: 'Open a project', detail: 'Choose the code AVA works on. Its chats appear in the sidebar.', action: { label: 'Open', name: 'Open a project', run: onOpenProject } },
    { id: 'message', done: state.firstMessageSent, title: 'Ask AVA for a change', detail: 'In a chat, type what you want in the message box and send it.' },
  ];
  const done = steps.filter((step) => step.done).length;
  const complete = done === steps.length;
  const toggle = collapsible && complete;
  const folded = toggle && !expanded;
  // Only the next step to do carries the primary action, so the eye lands on one thing.
  // A host returning focus after a dialog lands on `data-next-step`, else (all done) on the heading.
  const next = steps.find((step) => !step.done);
  const heading = <Heading id={`${id}-heading`} className={styles.heading} tabIndex={-1} data-steps-heading>Getting started</Heading>;
  return (
    <section className={styles.root} aria-labelledby={`${id}-heading`}>
      {/* The toggle keeps its place in both states, so focus stays on it. */}
      {toggle ? <div className={styles.head} data-folded={folded}>
        {heading}
        {folded ? <p className={styles.setUp}><CheckCircleIcon size={14} weight="fill" aria-hidden="true" /><span>You’re set up.</span></p> : null}
        <button type="button" className={styles.toggle} aria-expanded={expanded} aria-controls={expanded ? `${id}-steps` : undefined} onClick={() => setExpanded((value) => !value)}>
          {expanded ? 'Hide steps' : 'Show steps'}<CaretDownIcon size={12} aria-hidden="true" />
        </button>
      </div> : heading}
      {folded ? null : <>
        <ol id={`${id}-steps`} className={styles.steps}>
          {steps.map((step) => (
            <li key={step.id} className={styles.step} data-done={step.done}>
              <span className={styles.status}>
                {step.done ? <CheckCircleIcon size={14} weight="fill" aria-hidden="true" /> : <CircleIcon size={14} aria-hidden="true" />}
                {step.done ? 'Done' : 'To do'}
              </span>
              <span className={styles.text}>
                <span className={styles.title}>{step.title}</span>
                <span className={styles.detail}>{step.detail}</span>
              </span>
              {step.action && !step.done ? (
                <Button className={styles.action} tone={step === next ? 'primary' : 'quiet'} data-next-step={step === next || undefined} aria-label={step.action.name} onClick={step.action.run}>{step.action.label}</Button>
              ) : null}
            </li>
          ))}
        </ol>
        <p className={styles.progress} role="status" data-complete={complete}>
          {complete ? 'You’re set up.' : `${done} of ${steps.length} done`}
        </p>
      </>}
    </section>
  );
}
