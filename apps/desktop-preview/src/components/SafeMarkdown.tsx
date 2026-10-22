import { createContext, memo, useContext, useEffect, useRef, useState, type ComponentProps } from 'react';
import Markdown, { type Components, type ExtraProps } from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { IncrementalText } from '../chat/widgets/IncrementalText';
import { paintText } from '../chat/textPaint';
import { CodeBlock } from './CodeBlock';
import styles from './Conversation.module.css';

const markdownPlugins = [remarkGfm];

// The streaming tail's unterminated fence skips highlighting. A context, not a second components
// map, carries that: one stable `pre` type lets React keep the block's DOM when the fence closes.
const OpenFence = createContext(false);

// Fenced and indented code share the framed CodeBlock; its text stays a React-escaped string.
function FencedCode({ node, children }: ComponentProps<'pre'> & ExtraProps) {
  const partial = useContext(OpenFence);
  const code = node?.children[0];
  if (code?.type !== 'element' || code.tagName !== 'code') return <pre>{children}</pre>;
  const classes = code.properties.className;
  const language = Array.isArray(classes) ? classes.map(String).find((name) => name.startsWith('language-'))?.slice(9) : undefined;
  const text = code.children.map((child) => (child.type === 'text' ? child.value : '')).join('').replace(/\n$/, '');
  return <CodeBlock code={text} language={language} partial={partial} />;
}

const components: Components = {
  img: ({ alt }) => <span>{alt ? `[Image: ${alt}]` : '[Image omitted]'}</span>,
  pre: FencedCode,
};

const fenceLine = /^ {0,3}(`{3,}|~{3,})(.*)$/;
const fenceMarker = /^ {0,3}(?:`{3,}|~{3,})/;
const arrivingFence = /^ {0,3}(?:`{1,2}|~{1,2}|`{3,}[^`]*|~{3,}.*)$/;
const listItem = /^(?:[-+*]|\d{1,9}[.)])(?:[ \t]|$)/;
const definition = /^ {0,3}\[[^\]\n]+\]:/m;

type Blocks = { parts: string[]; fenced: boolean };

// Splits only where CommonMark starts an independent top-level block (a blank line, then an
// unindented line that does not continue a list, outside any fence), so each part renders exactly
// as it would in the whole document and settled parts keep their DOM while a reply streams.
// Link and footnote definitions resolve across blocks, so their presence keeps one part.
function blocks(text: string): Blocks {
  const lines = text.split('\n');
  const split = !definition.test(text);
  const parts: string[] = [];
  let start = 0;
  let fence = '';
  let blank = false;
  let list = false;
  let content = false;
  lines.forEach((line, index) => {
    if (fence) {
      const close = fenceLine.exec(line);
      if (close && close[1].startsWith(fence) && !close[2].trim()) fence = '';
      return;
    }
    if (!line.trim()) { blank = true; return; }
    if (split && blank && content && !/^[ \t]/.test(line) && !(list && listItem.test(line))) {
      parts.push(lines.slice(start, index).join('\n'));
      start = index;
      list = false;
    }
    blank = false;
    content = true;
    if (listItem.test(line)) list = true;
    const open = fenceLine.exec(line);
    if (open && !(open[1][0] === '`' && open[2].includes('`'))) fence = open[1];
  });
  parts.push(lines.slice(start).join('\n'));
  return { parts, fenced: fence !== '' };
}

// Closes inline syntax the open paragraph has started but not finished, so `**`, a backtick or a
// half-written link never flashes as literal text. Markers still arriving at the end are hidden.
function balance(run: string): string {
  const open: string[] = [];
  let code = 0;
  let cut = run.length;
  for (let index = 0; index < run.length;) {
    const char = run[index];
    if (char === '\\' && !code) { index += 2; continue; }
    if (char !== '`' && char !== '*' && char !== '~') { index += 1; continue; }
    let size = 1;
    while (run[index + size] === char) size += 1;
    const last = index + size === run.length;
    if (char === '`') {
      if (code === size) code = 0;
      else if (!code) {
        if (last) cut = index;
        else code = size;
      }
    } else if (!code) {
      const before = index ? run[index - 1] : ' ';
      const after = run[index + size] ?? '';
      const marks = char === '~' ? (size === 2 ? ['~~'] : []) : size === 1 ? ['*'] : size === 2 ? ['**'] : size === 3 ? ['*', '**'] : [];
      for (const mark of marks) {
        const opened = open.lastIndexOf(mark);
        if (opened >= 0 && /\S/.test(before)) open.splice(opened, 1);
        else if (last) cut = Math.min(cut, index);
        else if (/\S/.test(after) && /[\s\p{P}]/u.test(before)) open.push(mark);
      }
    }
    index += size;
  }
  let body = run.slice(0, cut);
  if (!code) body = body.replace(/(!?)\[([^\]\n]*)\]\([^)\s]*$/, (_link, image: string, label: string) => (image ? '' : label));
  const text = body.trimEnd();
  const closers = (code && text.endsWith('`') ? ' ' : '') + '`'.repeat(code) + open.reverse().join('');
  return closers ? text + closers + body.slice(text.length) : body;
}

function closeInline(block: string): string {
  const end = block.endsWith('\n') ? block.length - 1 : block.length;
  let start = block.lastIndexOf('\n', end - 1) + 1;
  while (start > 0) {
    const previous = block.lastIndexOf('\n', start - 2) + 1;
    const line = block.slice(previous, start - 1);
    if (!line.trim() || fenceMarker.test(line)) break;
    start = previous;
  }
  const run = block.slice(start, end);
  if (!run.trim() || fenceMarker.test(run) || /^(?: {4}|\t)/.test(run)) return block;
  return block.slice(0, start) + balance(run) + block.slice(end);
}

function present(text: string, streaming: boolean): Blocks {
  if (!streaming) return blocks(text);
  // A fence marker still arriving would flash as literal backticks or a half-typed language name.
  const lineStart = text.lastIndexOf('\n') + 1;
  const view = blocks(arrivingFence.test(text.slice(lineStart)) ? text.slice(0, lineStart) : text);
  if (!view.fenced) view.parts[view.parts.length - 1] = closeInline(view.parts[view.parts.length - 1]);
  return view;
}

const MarkdownBlock = memo(function MarkdownBlock({ text, openFence }: { text: string; openFence: boolean }) {
  return <OpenFence value={openFence}><Markdown remarkPlugins={markdownPlugins} components={components} skipHtml>{text}</Markdown></OpenFence>;
});

function selectionWithin(node: HTMLElement | null): boolean {
  const selection = window.getSelection();
  return !!node && !!selection && !selection.isCollapsed && (node.contains(selection.anchorNode) || node.contains(selection.focusNode));
}

export const SafeMarkdown = memo(function SafeMarkdown({ children, partial = false }: { children: string; partial?: boolean }) {
  const text = paintText(children);
  const root = useRef<HTMLDivElement>(null);
  const [shown, setShown] = useState({ text, partial });
  const current = shown.text === text && shown.partial === partial;
  // Rendering again would rewrite text nodes under the reader's selection: keep the painted view,
  // append newer supplied text after it, and format everything once the selection ends.
  const held = !current && text.startsWith(shown.text) && selectionWithin(root.current);
  if (!current && !held) setShown({ text, partial });
  useEffect(() => {
    if (!held) return;
    const release = () => { if (!selectionWithin(root.current)) setShown({ text, partial }); };
    release();
    document.addEventListener('selectionchange', release);
    return () => document.removeEventListener('selectionchange', release);
  }, [held, text, partial]);
  const view = held ? shown : { text, partial };
  const { parts, fenced } = present(view.text, view.partial);
  return <div ref={root}>
    {parts.map((part, index) => <MarkdownBlock key={index} text={part} openFence={view.partial && fenced && index === parts.length - 1} />)}
    {held && text.length > shown.text.length ? <div className={styles.partialText}><IncrementalText text={text.slice(shown.text.length)} /></div> : null}
    {children.length > 20000 || children.split('\n').length > 200 ? <p className={styles.note}>Response display limited to 200 lines or 20,000 characters.</p> : null}
  </div>;
});
