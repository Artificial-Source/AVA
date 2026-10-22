import { useLayoutEffect, useRef, useState, type KeyboardEvent, type MouseEvent } from 'react';
import { canFocus } from '../ui/DialogFrame';

function atBottom(node: HTMLElement) { return node.scrollHeight - node.clientHeight - node.scrollTop <= 24; }
export function useBottomFollow<T extends HTMLElement = HTMLDivElement>(value: unknown, active = true, enabled = true) {
  const ref = useRef<T>(null);
  const latestRef = useRef<HTMLButtonElement>(null);
  const follow = useRef(false);
  const reading = useRef(false);
  const latestFocus = useRef(false);
  const [away, setAway] = useState(false);
  const current = useRef({ value, active, enabled });
  // A user-opened disclosure is reading intent: stop pinning so the opened row stays where it was.
  // The hold outlives programmatic scrolls (revealing the row, lazy details growing) and ends only
  // when the user scrolls the pane themselves or chooses Latest.
  const held = useRef(false);
  const hold = useRef(() => { held.current = true; follow.current = false; latestFocus.current = false; }).current;
  if (current.current.value !== value || current.current.active !== active || current.current.enabled !== enabled) current.current = { value, active, enabled };
  const snapshot = current.current;
  function visible(node: HTMLElement) { return current.current.active && !document.hidden && !document.querySelector('dialog[open]') && canFocus(node); }
  function update() {
    const node = ref.current;
    if (!node || !visible(node) || !current.current.enabled) return;
    const selection = window.getSelection();
    const ownLatestFocus = latestFocus.current && document.activeElement === node;
    if (follow.current && !reading.current && (!selection || selection.isCollapsed) && (!node.contains(document.activeElement?.closest('pre') ?? null) || ownLatestFocus)) node.scrollTop = node.scrollHeight;
    if (!atBottom(node)) { follow.current = false; latestFocus.current = false; }
    setAway(!atBottom(node));
  }
  useLayoutEffect(() => {
    const node = ref.current!;
    follow.current = atBottom(node);
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(update);
    observer?.observe(node);
    if (node.firstElementChild) observer?.observe(node.firstElementChild);
    const release = () => { reading.current = false; };
    window.addEventListener('pointerup', release); window.addEventListener('pointercancel', release); window.addEventListener('keyup', release);
    return () => { observer?.disconnect(); window.removeEventListener('pointerup', release); window.removeEventListener('pointercancel', release); window.removeEventListener('keyup', release); };
  }, []);
  useLayoutEffect(update, [value]);
  return { ref, latestRef, away, hold, onPointerDown: () => { held.current = false; reading.current = true; latestFocus.current = false; }, onWheel: () => { held.current = false; }, onKeyDown: (event: KeyboardEvent<HTMLElement>) => {
    // Keyboard activation reads like a press: no growth it causes may pin the pane meanwhile.
    if (event.key === 'Enter' || event.key === ' ') { reading.current = true; latestFocus.current = false; }
    else if (['PageUp', 'PageDown', 'Home', 'End', 'ArrowUp', 'ArrowDown'].includes(event.key)) held.current = false;
  }, onBlur: () => { latestFocus.current = false; }, onScroll: () => {
    const node = ref.current;
    if (node && visible(node)) {
      follow.current = atBottom(node) && !held.current;
      if (!follow.current) latestFocus.current = false;
      setAway(!atBottom(node));
    }
  }, latest: (event: MouseEvent<HTMLButtonElement>) => {
    const node = ref.current; const control = event.currentTarget;
    if (!node || current.current !== snapshot || !current.current.enabled || !visible(node) || control !== latestRef.current || !canFocus(control) || event.defaultPrevented) return;
    reading.current = false; held.current = false; follow.current = true; latestFocus.current = false;
    node.scrollTop = node.scrollHeight;
    // Only this explicit control may hand its own focus back before it disappears.
    if (document.activeElement === control) {
      node.focus({ preventScroll: true });
      latestFocus.current = document.activeElement === node;
    }
    setAway(false);
  } };
}
