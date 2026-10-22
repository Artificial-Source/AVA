import type { SettingsSection } from './contracts';
import { shortcutActions, type ShortcutAction } from './shortcuts';

// Row-level search index. Each id is also the `data-setting` anchor on the rendered row.
const rows = [
  { id: 'general.enter', section: 'general', title: 'Enter key', description: 'Send with Enter, or use Enter for a new line.', keywords: 'send newline return submit keyboard compose message' },
  { id: 'general.mode', section: 'general', title: 'Default mode', description: 'New chats start in Build or Plan.', keywords: 'build plan new chat' },
  { id: 'general.project', section: 'general', title: 'Project', description: 'Where new chats are created.', keywords: 'new chat folder workspace repository' },
  { id: 'general.title', section: 'general', title: 'Name from first message', description: 'Title new chats from your first message.', keywords: 'rename naming title new chat' },
  { id: 'general.inspector', section: 'general', title: 'Inspector on new chats', description: 'Remember the current layout, or start with a tab open.', keywords: 'layout panel agents changes context' },
  { id: 'general.resetAll', section: 'general', title: 'Reset all preferences', description: 'Restore every preference and the default model.', keywords: 'defaults restore factory clear' },
  { id: 'appearance.palette', section: 'appearance', title: 'Palette', description: 'Dark only. Choose Default, Charcoal or Midnight.', keywords: 'theme dark mode light mode color colour charcoal midnight background shade' },
  { id: 'appearance.contrast', section: 'appearance', title: 'Contrast', description: 'Stronger text and separators.', keywords: 'accessibility high contrast separators legibility' },
  { id: 'appearance.font', section: 'appearance', title: 'Interface font', description: 'Code keeps its monospace font.', keywords: 'typography typeface geist system sans' },
  { id: 'appearance.proseSize', section: 'appearance', title: 'Conversation text', description: 'Size of message text.', keywords: 'font size text size zoom larger smaller prose' },
  { id: 'appearance.spacing', section: 'appearance', title: 'Line spacing', description: 'Space between lines of message text.', keywords: 'line height leading prose' },
  { id: 'appearance.width', section: 'appearance', title: 'Reading width', description: 'Maximum width of the conversation and composer.', keywords: 'column measure narrow wide' },
  { id: 'appearance.material', section: 'appearance', title: 'Sidebar material', description: 'Glass or opaque sidebar.', keywords: 'glass opaque transparency blur translucent' },
  { id: 'appearance.motion', section: 'appearance', title: 'Motion', description: 'Reduce movement across the app.', keywords: 'animation reduce motion accessibility' },
  { id: 'appearance.density', section: 'appearance', title: 'Settings-row density', description: 'Spacing between Settings rows.', keywords: 'compact comfortable spacing' },
  { id: 'shortcuts.enabled', section: 'shortcuts', title: 'App shortcuts', description: 'Turn all app keyboard shortcuts on or off.', keywords: 'keyboard hotkeys keybindings enable disable' },
  { id: 'notifications.mute', section: 'notifications', title: 'Mute all notifications', description: 'Silence every notification and sound.', keywords: 'silence quiet do not disturb sound' },
  { id: 'notifications.background', section: 'notifications', title: 'Background only', description: 'Notify only while AVA isn’t the active window.', keywords: 'focus foreground window' },
  { id: 'notifications.completion', section: 'notifications', title: 'Task finished', description: 'When AVA finishes a task.', keywords: 'alert sound tone chime notify done complete' },
  { id: 'notifications.input', section: 'notifications', title: 'Needs attention', description: 'When AVA needs your answer or approval.', keywords: 'alert sound tone chime notify input permission question waiting' },
  { id: 'notifications.failure', section: 'notifications', title: 'Task failed', description: 'When a task stops with an error.', keywords: 'alert sound tone chime notify error failure' },
  { id: 'notifications.volume', section: 'notifications', title: 'Volume', description: 'Loudness of notification sounds.', keywords: 'loudness sound audio' },
  { id: 'models.default', section: 'models', title: 'Default model', description: 'The model new chats start with.', keywords: 'model new chat llm aster orion' },
  { id: 'models.providers', section: 'models', title: 'Providers', description: 'Connect a provider with an API key or sign-in.', keywords: 'api key credentials login sign in account connect disconnect provider' },
  { id: 'conversation.toolDetails', section: 'conversation', title: 'Tool details', description: 'How much tool activity shows in a chat.', keywords: 'compact auto expanded tool activity collapse expand' },
  { id: 'conversation.output', section: 'conversation', title: 'Show completed-tool output', description: 'Errors, status and file changes always stay visible.', keywords: 'hide results output' },
  { id: 'conversation.wrapCode', section: 'conversation', title: 'Wrap code lines', description: 'Wrap long lines in code blocks.', keywords: 'soft wrap code blocks' },
  { id: 'conversation.wrapDiff', section: 'conversation', title: 'Wrap diff lines', description: 'Wrap long lines in diffs and Changes.', keywords: 'diff changes wrap' },
  { id: 'conversation.scroll', section: 'conversation', title: 'After sending a message', description: 'Scroll to your message, or keep your reading position.', keywords: 'scroll jump latest auto-scroll' },
  { id: 'context.auto', section: 'context', title: 'Automatic compaction', description: 'Summarize older turns when the context window fills up.', keywords: 'compact summarize context window' },
  { id: 'context.trigger', section: 'context', title: 'Compact at', description: 'Share of the context window that starts compaction.', keywords: 'threshold trigger percentage compaction' },
  { id: 'context.turns', section: 'context', title: 'Recent turns to keep', description: 'Turns kept word for word after compaction.', keywords: 'history retention' },
  { id: 'context.tokens', section: 'context', title: 'Recent token budget', description: 'Tokens kept word for word after compaction.', keywords: 'tokens retention budget' },
  { id: 'permissions.trust', section: 'permissions', title: 'Project trust', description: 'Whether AVA uses a project’s own skills, prompts and servers.', keywords: 'trust untrusted /trust workspace project resources extensions subagents plugins mcp lsp language servers' },
  { id: 'permissions.rules', section: 'permissions', title: 'Rules and grants', description: 'What AVA may do without asking.', keywords: 'trust allow deny approval permission policy session' },
  { id: 'subagents.list', section: 'subagents', title: 'Subagents', description: 'Create, edit or delete your subagents.', keywords: 'agents delegate new create edit delete model' },
  { id: 'extensions.skills', section: 'extensions', title: 'Skills', description: 'Skills available to AVA.', keywords: 'extension' },
  { id: 'extensions.prompts', section: 'extensions', title: 'Prompts', description: 'Saved prompts available to AVA.', keywords: 'extension templates' },
  { id: 'extensions.plugins', section: 'extensions', title: 'Plugins', description: 'Installed plugins.', keywords: 'extension packages' },
  { id: 'extensions.mcp', section: 'extensions', title: 'MCP servers', description: 'Model Context Protocol servers.', keywords: 'extension model context protocol stdio tools' },
  { id: 'extensions.lsp', section: 'extensions', title: 'Language servers', description: 'Language servers for code intelligence.', keywords: 'extension lsp diagnostics' },
  { id: 'about.preview', section: 'about', title: 'About AVA', description: 'What this preview is.', keywords: 'version build preview' },
] as const satisfies readonly Readonly<{ id: string; section: SettingsSection; title: string; description: string; keywords: string }>[];

export type SettingId = (typeof rows)[number]['id'] | `shortcuts.${ShortcutAction}`;
export type SettingRow = Readonly<{ id: SettingId; section: SettingsSection; title: string; description: string; keywords: string }>;
const shortcutRows = shortcutActions.map((action): SettingRow => ({ id: `shortcuts.${action.id}`, section: 'shortcuts', title: action.label, description: action.detail, keywords: `shortcut keyboard keybinding hotkey ${action.group}` }));
export const settingRows: readonly SettingRow[] = [...rows, ...shortcutRows];

// Spread onto the element a search result scrolls to and highlights.
export function anchor(id: SettingId) { return { 'data-setting': id }; }

// Every word must appear in the row's title, description, keywords or page name.
export function matchSettings<S extends Readonly<{ id: SettingsSection; label: string }>>(query: string, sections: readonly S[]) {
  const words = query.toLocaleLowerCase().split(/\s+/u).filter(Boolean);
  if (!words.length) return [];
  return sections.map((section) => ({
    section,
    rows: settingRows.filter((row) => {
      if (row.section !== section.id) return false;
      const text = `${row.title} ${row.description} ${row.keywords} ${section.label}`.toLocaleLowerCase();
      return words.every((word) => text.includes(word));
    }),
  })).filter((group) => group.rows.length > 0);
}
