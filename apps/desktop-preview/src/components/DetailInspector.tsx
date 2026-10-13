import type { Chat } from '../state/types';
import { ContextInfo, type ContextInfoView, type LocalMessageProjection } from '../chat/widgets/ContextInfo';
import type { CumulativeUsageView } from '../chat/contextUsage';
import { ChangeReview } from './ChangeReview';
import { previewContextUsage } from './contextPreview';

// Optional inspector bodies load only when Context or Changes is requested.
// The shell owns visibility, parent identity, native focus and the retained exit.
export default function DetailInspector({ module, chat, active }: { module: 'changes' | 'context'; chat: Chat; active: boolean }) {
  return module === 'changes' ? <ChangeReview chat={chat} /> : <ContextInfo key={chat.id} view={previewContextInfo(chat)} active={active} />;
}

// Cumulative session samples are independent of the selected model's active estimate.
// Reasoning/cache may be subsets; reported totals are never reconstructed from them.
const cumulative: Readonly<Record<string, CumulativeUsageView>> = {
  'search-filters': { input: 256400, output: 18920, reasoning: 3640, cacheRead: 112000, cacheWrite: 8200, total: 275320, cost: { usd: 1.24, completeness: 'complete' }, capturedAt: '18 Jun 2026, 10:42 UTC' },
  'keyboard-navigation': { input: 68400, output: 3600, reasoning: 1200, cacheRead: 24000, cacheWrite: 0, total: 72000, cost: { usd: 0.18, completeness: 'partial' }, capturedAt: '18 Jun 2026, 10:38 UTC' },
};

export function previewContextInfo(chat: Chat): ContextInfoView {
  const messages: LocalMessageProjection[] = chat.messages.map((message) => message.role === 'user' ? {
    id: message.id, role: message.role, origin: message.origin === 'local' ? 'local' : 'example', text: message.text,
  } : {
    id: message.id, role: message.role, origin: 'example', text: message.markdown,
    ...(message.code ? { code: { filename: message.code.filename, text: message.code.code } } : {}),
    ...(message.note ? { note: message.note } : {}),
  });
  return {
    sessionTitle: chat.title,
    qualifier: 'Fictional profiles and fixed usage snapshots, not live measurements. Invented prompt and committed local message projection—not a sent prompt or provider payload.',
    usage: previewContextUsage(chat),
    cumulative: cumulative[chat.id] ?? {},
    systemPrompt: 'You are AVA in a fictional workspace. Explain proposed changes clearly, distinguish suggestions from verified results, and ask before taking action. This local example has no tool or provider connection.',
    messages,
  };
}
