import { Button } from '../ui/Button';
import { toolDetailsOpen } from '../chat/toolDisplay';
import { ToolDisplaySelect } from '../chat/widgets/ToolDisplaySelect';
import { defaultConversation, type ConversationPreferences } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export function ConversationSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.conversation ?? defaultConversation;
  function change(patch: Partial<ConversationPreferences>) { if (allowed()) update({ conversation: { ...value, ...patch } }); }
  const changed = JSON.stringify(value) !== JSON.stringify(defaultConversation);
  return <div>
    <fieldset className={styles.group} disabled={!active}><legend>Tool activity</legend>
      <div {...anchor('conversation.toolDetails')}><PreferenceRow title="Tool details" description="Compact shows one line per tool. Auto opens running and failed tools and folds finished ones. Expanded opens everything. Choosing a preset shows completed output again."><ToolDisplaySelect className={controls.select} display={value.toolDisplay} showCompletedOutput={value.showCompletedOutput} disabled={!active} onChange={(toolDisplay) => change({ toolDisplay, showCompletedOutput: true })} /></PreferenceRow></div>
      <div {...anchor('conversation.output')}><PreferenceRow title="Show completed-tool output" description="Errors, status and file changes always stay visible."><input type="checkbox" aria-label="Show completed-tool output" checked={value.showCompletedOutput} onChange={(event) => change({ showCompletedOutput: event.currentTarget.checked })} /></PreferenceRow></div>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Reading</legend>
      <div {...anchor('conversation.wrapCode')}><PreferenceRow title="Wrap code lines" description="Wrap long lines in code blocks, in chats and subagent views."><input type="checkbox" aria-label="Wrap code lines" checked={value.wrapCode} onChange={(event) => change({ wrapCode: event.currentTarget.checked })} /></PreferenceRow></div>
      <div {...anchor('conversation.wrapDiff')}><PreferenceRow title="Wrap diff lines" description="Wrap long lines in diffs, in chats and in Changes."><input type="checkbox" aria-label="Wrap diff lines" checked={value.wrapDiff} onChange={(event) => change({ wrapDiff: event.currentTarget.checked })} /></PreferenceRow></div>
      <div {...anchor('conversation.scroll')}><PreferenceRow title="After sending a message" description="Scroll to your message, or keep your reading position."><select className={controls.select} aria-label="After sending a message" value={value.scrollAfterSend ? 'latest' : 'keep'} onChange={(event) => { const next = event.currentTarget.value; if (next === 'latest' || next === 'keep') change({ scrollAfterSend: next === 'latest' }); }}><option value="latest">Scroll to message</option><option value="keep">Keep reading position</option></select></PreferenceRow></div>
    </fieldset>
    <figure className={styles.sample} aria-label="Live conversation preview"><figcaption>Preview</figcaption><p>Review the change without losing your place.</p><pre data-wrap={value.wrapCode} aria-label="Code wrap sample"><code>{'const title = firstMessage.trim().replace(/\\s+/gu, " "); // collapse whitespace before naming the chat'}</code></pre>
      <details key={`${value.toolDisplay}:running-sample`} open={toolDetailsOpen(value.toolDisplay, 'running')}><summary>Running tool</summary><p>Reading notes.md… Auto folds this when it finishes, unless you open it.</p></details>
      <details key={`${value.toolDisplay}:completed-sample`} open={toolDetailsOpen(value.toolDisplay, 'completed')}><summary>Completed tool</summary>{value.showCompletedOutput ? <p>Found two headings in notes.md.</p> : <p>Output hidden. Status and file changes stay visible.</p>}<pre data-wrap={value.wrapDiff} aria-label="Diff wrap sample"><code>{'+ Keep this long diff line readable without changing what it says or where it came from.'}</code></pre></details>
      <details key={`${value.toolDisplay}:failed-sample`} open={toolDetailsOpen(value.toolDisplay, 'failed')}><summary>Failed tool</summary><p>Couldn’t read notes.md. Auto keeps failures open.</p></details>
    </figure>
    <div className={styles.reset}><p>Restore tool activity and reading.</p><Button disabled={!active || !changed} onClick={() => { if (changed && allowed()) update({ conversation: defaultConversation }); }}>Reset conversation</Button></div>
  </div>;
}
