import type { Dispatch } from 'react';
import { ChatComposer } from '../chat/widgets/ChatComposer';
import { contextReferences, modelOptions } from '../state/fixtures';
import type { Chat, PreviewAction } from '../state/types';
import { ModelMark } from './ModelMark';
import { previewContextUsage } from './contextPreview';

const modeOptions = [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }] as const;
const copy = {
  context: 'Example context', contextDescription: 'Invented references only. Nothing is opened or uploaded.',
  addContext: 'Add example context', attachedContext: 'Attached example context', contextTitle: 'Choose invented context references; no files are read',
  referencePrefix: 'Invented', modelTitle: 'Fictional model catalog; no provider is connected',
  stop: 'Stop mock work',
};

export function Composer({ chat, dispatch, contextOpen, onOpenContext, onOpenModel, enterBehavior = 'newline' }: { chat: Chat; dispatch: Dispatch<PreviewAction>; contextOpen: boolean; onOpenContext: () => void; onOpenModel?: () => void; enterBehavior?: 'send' | 'newline' }) {
  return <ChatComposer
    key={chat.id}
    idPrefix=""
    view={{ title: chat.title, draft: chat.draft, expanded: chat.composerExpanded, runState: chat.status === 'working' ? 'running' : 'idle', contextOptions: contextReferences, selectedContextIds: chat.contextIds, modelOptions, modelId: chat.model, modeOptions, modeId: chat.mode, contextUsage: previewContextUsage(chat) }}
    copy={{ ...copy, stop: chat.messages.some((message) => message.role === 'assistant' && message.lifecycle === 'streaming') ? 'Stop preview' : copy.stop, sendTitle: `Add a local message. No model call. ${enterBehavior === 'send' ? 'Enter or ' : ''}Ctrl/Cmd+Enter.` }}
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
