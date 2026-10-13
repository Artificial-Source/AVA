import { memo, useEffect, useRef, useState } from 'react';
import { IncrementalText } from '../chat/widgets/IncrementalText';
import Markdown, { type Components } from 'react-markdown';
import { paintText } from '../chat/textPaint';
import styles from './Conversation.module.css';
import remarkGfm from 'remark-gfm';

const markdownPlugins = [remarkGfm];

const components: Components = { img: ({ alt }) => <span>{alt ? `[Image: ${alt}]` : '[Image omitted]'}</span> };

export const SafeMarkdown = memo(function SafeMarkdown({ children, partial = false }: { children: string; partial?: boolean }) {
  const text = paintText(children);
  const root = useRef<HTMLDivElement>(null);
  const [plain, setPlain] = useState(partial);
  if (partial && !plain) setPlain(true);
  function selected() { const selection = window.getSelection(); return !!selection && !selection.isCollapsed && !!root.current && (root.current.contains(selection.anchorNode) || root.current.contains(selection.focusNode)); }
  useEffect(() => {
    if (partial || !plain) return;
    const finish = () => { if (!selected()) setPlain(false); };
    finish(); document.addEventListener('selectionchange', finish);
    return () => document.removeEventListener('selectionchange', finish);
  }, [partial, plain]);
  return <div ref={root}>{partial || plain && selected() ? <div className={styles.partialText}><IncrementalText text={text} /></div> : <Markdown remarkPlugins={markdownPlugins} components={components} skipHtml>{text}</Markdown>}{children.length > 20000 || children.split('\n').length > 200 ? <p className={styles.note}>Response display limited to 200 lines or 20,000 characters.</p> : null}</div>;
});
