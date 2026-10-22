import type { TurnAttribution } from '../chat/contracts';
import type { ContextReference, ModelId, PreviewState } from './types';

// Invented examples only. These are not sessions, files, models, or live runs.
export const modelOptions: { id: ModelId; label: string }[] = [
  { id: 'aster', label: 'Aster' },
  { id: 'aster-mini', label: 'Aster Mini' },
  { id: 'orion', label: 'Orion' },
];

export const contextReferences: ContextReference[] = [
  { id: 'search-file', kind: 'file', label: 'SearchPage.tsx' },
  { id: 'result-image', kind: 'image', label: 'result-layout.png' },
  { id: 'navigation-notes', kind: 'context', label: 'Navigation notes' },
];

const asterAttribution: TurnAttribution = { modelLabel: 'Aster' };
const miniAttribution: TurnAttribution = { modelLabel: 'Aster Mini' };
const orionAttribution: TurnAttribution = { modelLabel: 'Orion' };

export function createInitialState(): PreviewState {
  return {
    projects: [
      { id: 'wayfinder', name: 'Wayfinder' },
      { id: 'papertrail', name: 'Papertrail' },
      { id: 'relay', name: 'Relay' },
    ],
    chats: [
      {
        id: 'search-filters',
        projectId: 'wayfinder',
        title: 'Remember search filters',
        status: 'idle',
        draft: 'Add a regression test for returning from a result.',
        model: 'aster',
        mode: 'build',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'search-question',
            role: 'user',
            origin: 'fixture',
            text: 'Keep the search filters when I open a result and go back. Use the URL so I can share the same view.',
          },
          {
            id: 'search-response',
            role: 'assistant',
            attribution: { modelLabel: 'Aster', thinkingLabel: 'High' },
            markdown:
              'The URL now owns the **query, sort order, and selected tags**. Opening a result leaves that state intact, so Back returns to the same view.',
            activity: [
              { id: 'read-search', action: 'Read source', detail: 'SearchPage.tsx and router.ts', state: 'completed' },
              { id: 'edit-search', action: 'Updated search state', detail: 'SearchPage.tsx and search-state.ts', state: 'completed', changes: [
                { id: 'search-state', filename: 'src/search-state.ts', operation: 'modified', additions: 2, deletions: 2, diff: { state: 'text', truncated: false, text: '@@ -1,2 +1,2 @@\n-const query = "";\n-const tags: string[] = [];\n+const query = params.get("q") ?? "";\n+const tags = params.getAll("tag");' } },
                { id: 'search-page', filename: 'src/SearchPage.tsx', operation: 'modified', additions: 1, deletions: 1, diff: { state: 'text', truncated: false, text: '@@ -1 +1 @@\n-const href = `/results/${id}`;\n+const href = `/results/${id}${location.search}`;' } },
              ] },
              { id: 'inspect-image', action: 'Inspect image', detail: 'Compare the pasted results page with the restored filters.', state: 'failed', resource: { kind: 'image', path: '/tmp/pi-clipboard-12345678.png', previewId: 'search-results-example' } },
            ],
            code: {
              filename: 'src/search-state.ts',
              code: 'const params = new URLSearchParams(location.search);\nconst query = params.get("q") ?? "";\nconst tags = params.getAll("tag");',
            },
            note: 'Browser back/forward behavior still needs a manual check.',
          },
        ],
        changes: [
          {
            filename: 'src/search-state.ts',
            description: 'Read the search state from URL parameters.',
            before: 'const query = "";\nconst tags: string[] = [];',
            after: 'const query = params.get("q") ?? "";\nconst tags = params.getAll("tag");',
          },
          {
            filename: 'src/SearchPage.tsx',
            description: 'Keep the same search parameters when opening a result.',
            before: 'const href = `/results/${id}`;',
            after: 'const href = `/results/${id}${location.search}`;',
          },
        ],
      },
      {
        id: 'keyboard-navigation',
        projectId: 'wayfinder',
        title: 'Keyboard navigation',
        status: 'working',
        draft: '',
        model: 'aster-mini',
        mode: 'build',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'keyboard-question',
            role: 'user',
            origin: 'fixture',
            text: 'Make the search results navigable with the keyboard, without moving focus away from the search input.',
          },
          {
            id: 'keyboard-response',
            role: 'assistant',
            attribution: miniAttribution,
            markdown:
              'I’m looking at how the result list tracks the active item. The intended behavior is **Arrow keys to explore, Enter to open, and Escape to return**.\n\nFocus should stay predictable when the result list changes.',
            activity: [
              { id: 'read-list', action: 'Read source', detail: 'ResultList.tsx and SearchInput.tsx', state: 'running' },
              { id: 'check-focus', action: 'Check focus order', detail: 'Arrow keys, Enter and Escape', state: 'queued' },
            ],
          },
        ],
        changes: [],
      },
      {
        id: 'route-config',
        projectId: 'wayfinder',
        title: 'Simplify route config',
        status: 'completed',
        draft: '',
        model: 'aster',
        mode: 'plan',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'route-question',
            role: 'user',
            origin: 'fixture',
            text: 'Find a simpler way to organize the public and account routes.',
          },
          {
            id: 'route-response',
            role: 'assistant',
            attribution: { modelLabel: 'Aster', thinkingLabel: 'Medium' },
            markdown:
              'Keep two small route groups with one shared layout.\n\n1. Keep public pages independent of account state.\n2. Put the account boundary around the account group.\n3. Preserve existing paths and redirects.\n\nThis is a proposed structure, not an implementation.',
          },
        ],
        changes: [],
      },
      {
        id: 'export-range',
        projectId: 'papertrail',
        title: 'Export date range',
        status: 'approval',
        draft: 'Keep the date range visible in the export summary.',
        model: 'orion',
        mode: 'plan',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'export-question',
            role: 'user',
            origin: 'fixture',
            text: 'Let me export only the entries in the selected date range.',
          },
          {
            id: 'export-response',
            role: 'assistant',
            attribution: orionAttribution,
            activity: [
              { id: 'read-export', action: 'Read source', detail: 'ExportDialog.tsx and date-range.ts', state: 'completed' },
            ],
            permission: {
              kind: 'permission',
              id: 'export-range-run',
              version: 1,
              state: 'ready',
              title: 'Run the export for the selected range?',
              summary: 'AVA wants to export the entries between the two dates and compare them with the visible list.',
              requester: 'AVA',
              action: 'npm run export -- --from 2026-10-01 --to 2026-10-14 --tz Europe/Berlin',
              target: 'papertrail/exports/range-check.csv',
              reason: 'Entries near midnight are where the timezone boundary usually goes wrong.',
              decisions: ['allow', 'allow_session', 'deny'],
              sessionAllowed: true,
              scopes: {
                allow: 'Only this command, this one time.',
                allow_session: 'Export commands in this chat until AVA restarts.',
                deny: 'AVA continues the plan without running the export.',
              },
            },
            markdown:
              'The export should use the same date boundaries as the visible list, including the selected timezone.\n\nThe proposed summary shows the **start date, end date, and included entries** before an export is prepared.',
          },
        ],
        changes: [],
      },
      {
        id: 'import-flow',
        projectId: 'papertrail',
        title: 'Document the import flow',
        status: 'stopped',
        draft: '',
        model: 'aster',
        mode: 'plan',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'import-question',
            role: 'user',
            origin: 'fixture',
            text: 'Outline a short guide for the import flow.',
          },
          {
            id: 'import-response',
            role: 'assistant',
            attribution: asterAttribution,
            activity: [{ id: 'import-outline', action: 'Read import notes', detail: 'import-guide.md', state: 'canceled' }],
            markdown:
              'A short guide can follow the flow itself:\n\n- Choose a supported source.\n- Review the column mapping.\n- Check a small sample before importing.\n\nCall out how duplicates are handled before the final step.',
          },
        ],
        changes: [],
      },
      {
        id: 'retry-behavior',
        projectId: 'relay',
        title: 'Review retry behavior',
        status: 'failed',
        draft: '',
        model: 'orion',
        mode: 'plan',
        contextIds: [],
        composerExpanded: false,
        messages: [
          {
            id: 'retry-question',
            role: 'user',
            origin: 'fixture',
            text: 'Review the retry policy for transient failures.',
          },
          {
            id: 'retry-response',
            role: 'assistant',
            attribution: orionAttribution,
            markdown:
              'Separate transient failures from permanent ones. Use a bounded retry count and a clear stopping point.\n\n| Failure | Proposed behavior |\n| --- | --- |\n| Temporary unavailable | Retry with backoff |\n| Invalid input | Explain; do not retry |\n| Cancelled | Stop immediately |\n\nI haven’t checked these against the current retry code yet.',
          },
        ],
        changes: [],
      },
    ],
    activeChatId: 'search-filters',
    pinnedChatIds: [],
    recentChatIds: ['search-filters'],
    collapsedProjectIds: [],
    // Deliberately not RPC snapshots: live child inspection needs a future protocol decision.
    childAgents: [
      {
        id: 'search-outline',
        parentChatId: 'search-filters',
        title: 'Map return paths',
        status: 'completed',
        model: 'aster-mini',
        timing: '42s',
        messages: [
          { id: 'outline-user', role: 'user', text: 'Outline the paths back to the filtered result list. Keep this to a short note.' },
          { id: 'outline-assistant', role: 'assistant', attribution: miniAttribution, markdown: 'There are two paths to consider: the **browser Back button** and the in-page return link. Both should keep the query, sort order, and selected tags.\n\nThis outline does not verify either path in a browser.' },
        ],
      },
      {
        id: 'search-return',
        parentChatId: 'search-filters',
        title: 'Review back navigation',
        status: 'working',
        model: 'aster',
        timing: '1m 12s',
        messages: [
          { id: 'return-user', role: 'user', text: 'Review how the selected filters should survive opening a result and returning to the list.' },
          { id: 'return-assistant', role: 'assistant', attribution: asterAttribution, markdown: 'The return path should preserve the **same search parameters**, rather than build a fresh search.\n\nA good test case starts with two tags and a non-default sort order. Open a result, return, and compare the visible controls with the URL. This is a suggested manual check, not a completed one.' },
        ],
      },
      {
        id: 'search-edge-cases',
        parentChatId: 'search-filters',
        title: 'Check empty queries',
        status: 'failed',
        model: 'orion',
        timing: 'Ended after 18s',
        messages: [
          { id: 'edge-user', role: 'user', text: 'Describe the expected return behavior when the query is empty but tags are selected.' },
          { id: 'edge-assistant', role: 'assistant', attribution: orionAttribution, markdown: 'I couldn’t open the source for this check, so I have **no verified result** to report.' },
        ],
      },
      {
        id: 'keyboard-focus',
        parentChatId: 'keyboard-navigation',
        title: 'Review focus boundaries',
        status: 'working',
        model: 'aster-mini',
        timing: '36s',
        messages: [
          { id: 'focus-user', role: 'user', text: 'Suggest a focus boundary for the search field and its result list.' },
          { id: 'focus-assistant', role: 'assistant', attribution: miniAttribution, markdown: 'Keep keyboard focus in the search field while the active result changes. Document what Escape does before choosing the final interaction.' },
        ],
      },
      {
        id: 'export-summary',
        parentChatId: 'export-range',
        title: 'Review summary wording',
        status: 'unavailable',
        model: 'orion',
        timing: 'No transcript',
        messages: [],
      },
    ],
    inspectedChildId: null,
    inspector: 'agents',
    lastInspectorModule: 'agents',
    opaque: false,
    nextChatNumber: 1,
    nextMessageNumber: 1,
    announcement: '',
  };
}
