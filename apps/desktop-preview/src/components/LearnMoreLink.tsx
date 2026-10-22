import type { ReactNode } from 'react';
import { QuestionIcon } from '@phosphor-icons/react/dist/csr/Question';
import type { HelpTopicId } from '../screens/contracts';
import { useOpenHelp } from './helpContext';
import styles from './LearnMoreLink.module.css';

// A quiet "About …" link into Help at one topic. Help opens in place, so this is a button that
// reads as a link. Without a Help opener it renders nothing: no dead links in isolated widgets.
// `className` only places it; the look stays the same everywhere.
export function LearnMoreLink({ topic, children, className = '' }: { topic: HelpTopicId; children: ReactNode; className?: string }) {
  const openHelp = useOpenHelp();
  if (!openHelp) return null;
  return (
    <button type="button" className={`${styles.link} ${className}`} aria-haspopup="dialog" data-help-topic={topic} onClick={() => openHelp(topic)}>
      <QuestionIcon size={13} aria-hidden="true" />
      <span>{children}</span>
    </button>
  );
}
