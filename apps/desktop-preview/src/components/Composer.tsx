import type { Dispatch } from 'react';
import { ChatComposer } from '../chat/widgets/ChatComposer';
import { applePlatform } from '../screens/shortcutBindings';
import { shortcutHint, type ShortcutPreferences } from '../screens/shortcuts';
import { contextReferences, modelOptions } from '../state/fixtures';
import type { Chat, PreviewAction } from '../state/types';
import { ModelMark } from './ModelMark';
import { previewContextUsage } from './contextPreview';

const modeOptions = [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }] as const;
const modeDescriptions = {
  build: 'AVA can edit files and run tools.',
  plan: 'AVA proposes changes without editing files.',
};
const copy = {
  context: 'Add context', addContext: 'Add context', attachedContext: 'Attached context', contextTitle: 'Add context',
  stop: 'Stop',
};

export function Composer({ chat, dispatch, contextOpen, onOpenContext, onOpenModel, enterBehavior = 'newline', shortcuts }: { chat: Chat; dispatch: Dispatch<PreviewAction>; contextOpen: boolean; onOpenContext: () => void; onOpenModel?: () => void; enterBehavior?: 'send' | 'newline'; shortcuts?: ShortcutPreferences }) {
  const modelHint = shortcutHint(shortcuts, 'models');
  const stopHint = shortcutHint(shortcuts, 'stop');
  const apple = applePlatform();
  const sendKeys = enterBehavior === 'send' ? apple ? '↩' : 'Enter' : apple ? '⌘↩' : 'Ctrl+Enter';
  return <ChatComposer
    key={chat.id}
    idPrefix=""
    view={{ title: chat.title, draft: chat.draft, expanded: chat.composerExpanded, runState: chat.status === 'working' ? 'running' : 'idle', contextOptions: contextReferences, selectedContextIds: chat.contextIds, modelOptions, modelId: chat.model, modeOptions, modeId: chat.mode, contextUsage: previewContextUsage(chat) }}
    copy={{
      ...copy,
      modelTitle: modelHint ? `Choose model · ${modelHint.label}` : 'Choose model',
      sendTitle: `Send · ${sendKeys}`,
      stopTitle: stopHint ? `Stop · ${stopHint.label}` : 'Stop',
    }}
    modeDescriptions={modeDescriptions}
    keyShortcuts={{ model: modelHint?.aria, stop: stopHint?.aria }}
    enterBehavior={enterBehavior}
    modelMark={<ModelMark model={chat.model} />}
    contextTarget={{ id: 'context-inspector', open: contextOpen }}
    onOpenContext={onOpenContext}
    onOpenModel={onOpenModel}
    onDraftChange={(draft) => dispatch({ type: 'setDraft', chatId: chat.id, draft })}
    onModelChange={(id) => {
      const model = modelOptions.find((option) => option.id === id);
      if (model) dispatch({ type: 'setModel', chatId: chat.id, model: model.id });
    }}
    onModeChange={(mode) => { if (mode === 'build' || mode === 'plan') dispatch({ type: 'setMode', chatId: chat.id, mode }); }}
    onExpandedChange={(expanded) => { if (expanded !== chat.composerExpanded) dispatch({ type: 'toggleComposerExpanded', chatId: chat.id }); }}
    onAddReference={(id) => {
      const reference = contextReferences.find((entry) => entry.id === id);
      if (reference) dispatch({ type: 'addContext', chatId: chat.id, contextId: reference.id });
    }}
    onRemoveReference={(id) => {
      const reference = contextReferences.find((entry) => entry.id === id);
      if (reference) dispatch({ type: 'removeContext', chatId: chat.id, contextId: reference.id });
    }}
    onSend={() => dispatch({ type: 'sendLocalMessage' })}
    onStop={() => dispatch({ type: 'stopViewedMock' })}
  />;
}
