import type { HelpTopicId, SettingsSection } from './contracts';

// Where a topic's Go to action leads: a Settings page, or Find a chat.
export type HelpDestination = SettingsSection | 'sessions';
// Getting started and Keyboard shortcuts are live sections; every other topic is a short article.
export type HelpArticleId = Exclude<HelpTopicId, 'getting-started' | 'shortcuts'>;
export type HelpArticle = Readonly<{
  id: HelpArticleId; title: string; body: readonly string[];
  // Searched with the title and body, never shown.
  keywords: string;
  go?: Readonly<{ label: string; to: HelpDestination }>;
}>;

// Two to four plain sentences each. Names match the controls they describe (the Composer's mode
// descriptions, the permission card's choices, Settings rows), so what Help says is what the app shows.
export const helpArticles: readonly HelpArticle[] = [
  {
    id: 'providers', title: 'Providers and API keys',
    body: [
      'AVA replies using models from the providers you connect. To connect one, sign in on its website, enter a one-time code on another device, or paste an API key, whichever that provider offers.',
      'API keys come from your provider account, and the provider bills their use. AVA checks a key with the provider before saving it.',
    ],
    keywords: 'provider providers connect connected disconnect api key keys token credentials account login log sign in signin billing billed models',
    go: { label: 'Go to Models & providers', to: 'models' },
  },
  {
    id: 'chats', title: 'Chats and projects',
    body: [
      'Each chat belongs to a project, and the sidebar lists your projects with their chats. New chat starts a chat with your default model and mode.',
      'Find a chat searches titles and messages in every project. Archiving a chat removes it from the sidebar without deleting it; it stays in Find a chat under Archived.',
    ],
    keywords: 'project projects sidebar new chat find search archive archived delete deleting remove branch rename pin recent history',
    go: { label: 'Go to Find a chat', to: 'sessions' },
  },
  {
    id: 'modes', title: 'Build and Plan',
    body: [
      'Each chat runs in Build or Plan, chosen with the mode control in the message box. In Build, AVA can edit files and run tools. In Plan, AVA proposes changes without editing files.',
      'New chats start in your default mode.',
    ],
    keywords: 'mode modes build plan edit files run tools propose read only default',
    go: { label: 'Go to General settings', to: 'general' },
  },
  {
    id: 'permissions', title: 'Permissions',
    body: [
      'When AVA needs your approval, it shows a request in the chat and waits. Allow once covers only that request, Allow for this session covers this kind of action in this chat until AVA restarts, and Deny lets AVA continue without doing it.',
      'Rules in Permissions & trust decide what AVA may do without asking.',
    ],
    keywords: 'allow deny approve approval request decision once session rules grants trust project trust',
    go: { label: 'Go to Permissions & trust', to: 'permissions' },
  },
  {
    id: 'changes', title: 'Changes',
    body: [
      'The Changes panel lists every file this chat changed, each as a diff you can open or fold. Open it with the Changes button at the right edge of the window, or choose Review in Changes on a file in the chat to jump straight to it.',
    ],
    keywords: 'diff diffs files edits edited changed review added removed lines panel',
  },
  {
    id: 'agents', title: 'Subagents',
    body: [
      'AVA can hand parts of a task to subagents, and the Agents panel lists the ones it started for this chat. Open one to read its conversation; it’s read only, and Back to parent returns you to the chat.',
      'You can create and edit your own subagents in Settings.',
    ],
    keywords: 'agents subagent child delegate delegated panel conversation read only',
    go: { label: 'Go to Subagent settings', to: 'subagents' },
  },
  {
    id: 'context', title: 'Context',
    body: [
      'Context is what the model reads with each message: the conversation so far, plus anything you attach with Add context in the message box. A model can only read so much at once, its context window.',
      'The Context panel shows how much of the window this chat uses. When the window fills up, AVA can summarize older turns to make room.',
    ],
    keywords: 'tokens window usage limit add context attach files compaction compact summarize cost',
    go: { label: 'Go to Context management', to: 'context' },
  },
  {
    id: 'tool-details', title: 'Tool details',
    body: [
      'AVA’s work shows in the chat as tool activity, such as reading files or running commands. Tool details sets how much of it shows: Compact shows one line per tool, Auto opens running and failed tools and folds finished ones, and Expanded opens everything.',
      'Expand all tools and Collapse all tools change every tool in the chat at once.',
    ],
    keywords: 'tools activity compact auto expanded expand collapse output running failed',
    go: { label: 'Go to Conversation behavior', to: 'conversation' },
  },
  {
    id: 'extensions', title: 'Extensions',
    body: [
      'Extensions lists the skills, prompts, plugins, MCP servers and language servers available to AVA.',
      'Until you trust a project, AVA skips that project’s own extensions. Permissions & trust shows how to trust one.',
    ],
    keywords: 'extension skill prompt templates plugin packages mcp model context protocol server servers tools lsp language diagnostics trust',
    go: { label: 'Go to Extensions', to: 'extensions' },
  },
  {
    id: 'appearance', title: 'Appearance',
    body: [
      'AVA is dark only, in three neutral palettes: Default, Charcoal and Midnight. Appearance also sets contrast, the interface font, the size and spacing of message text, reading width, the sidebar’s material and motion.',
    ],
    keywords: 'theme themes dark mode light color colour palette shade charcoal midnight contrast font typeface text size zoom larger bigger smaller line spacing reading width sidebar glass opaque transparency blur motion animation reduce',
    go: { label: 'Go to Appearance', to: 'appearance' },
  },
];

// Search text for the two live sections: what they show, plus words people search for.
export const gettingStartedSearch = 'getting started setup set up first start begin connect a provider sign in models open a project code sidebar ask ava for a change type message send';
export const shortcutsSearch = 'keyboard shortcuts shortcut keys hotkeys keybindings';
