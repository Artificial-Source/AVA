import type { ReactNode, Ref } from 'react';
import { QuestionIcon } from '@phosphor-icons/react/dist/csr/Question';
import { AvaMark } from './AvaMark';
import { GettingStarted, type GettingStartedState } from './GettingStarted';
import helpLink from './LearnMoreLink.module.css';
import styles from './Welcome.module.css';

// First run, while nothing is open: what AVA does and the steps that make it useful. It stands in
// for the conversation, message box and inspector; an opened project's empty chat stays logo-only.
// `aside` holds the collapsed preview banner's way back. Help is one quiet link under the steps, with
// the look of the contextual "About …" links and its fixed key beside it.
export function Welcome({ state, onConnectProvider, onOpenProject, onOpenHelp, headingRef, aside }: {
  state: GettingStartedState;
  onConnectProvider: () => void;
  onOpenProject: () => void;
  onOpenHelp: () => void;
  headingRef?: Ref<HTMLHeadingElement>;
  aside?: ReactNode;
}) {
  return (
    <div className={styles.welcome} data-welcome>
      {aside ? <div className={styles.aside}>{aside}</div> : null}
      <div className={styles.column}>
        <AvaMark className={styles.mark} />
        <h1 ref={headingRef} id="conversation-title" className={styles.heading} tabIndex={-1}>Welcome to AVA</h1>
        <p className={styles.lead}>Ask AVA for a change to your code, then review every edit it makes.</p>
        <GettingStarted state={state} onConnectProvider={onConnectProvider} onOpenProject={onOpenProject} headingLevel={2} />
        <button type="button" className={`${helpLink.link} ${styles.help}`} aria-haspopup="dialog" aria-keyshortcuts="F1" onClick={onOpenHelp}>
          <QuestionIcon size={13} aria-hidden="true" />
          <span>Help</span>{' '}<kbd className={styles.key}>F1</kbd>
        </button>
      </div>
    </div>
  );
}
