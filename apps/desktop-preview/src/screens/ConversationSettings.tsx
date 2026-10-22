import { Button } from '../ui/Button';
import { toolDetailsOpen } from '../chat/toolDisplay';
import { ToolDisplaySelect } from '../chat/widgets/ToolDisplaySelect';
import { defaultConversation, type ConversationPreferences } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export function ConversationSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.conversation ?? defaultConversation;
  function change(patch: Partial<ConversationPreferences>) { if (allowed()) update({ conversation: { ...value, ...patch } }); }
  const changed = JSON.stringify(value) !== JSON.stringify(defaultConversation);
  return <div>
    <p className={styles.intro}>Keep the conversation readable. These visit-local controls change how supplied content is shown, not messages, model reasoning or run status.</p>
    <fieldset className={styles.group} disabled={!active}><legend>Tool activity</legend>
      <PreferenceRow title="Chat display" description="Compact minimizes details. Auto opens running tools, folds finished work unless inspected, and keeps failed or unavailable details open. Expanded opens all. Presets restore output; a different preset replaces manual choices."><ToolDisplaySelect className={controls.select} display={value.toolDisplay} showCompletedOutput={value.showCompletedOutput} disabled={!active} onChange={(toolDisplay) => change({ toolDisplay, showCompletedOutput: true })} /></PreferenceRow>
      <PreferenceRow title="Completed-tool output" description="Errors, status and file changes remain visible. Only successful output text is hidden."><select className={controls.select} aria-label="Completed-tool output" value={value.showCompletedOutput ? 'show' : 'hide'} onChange={(event) => { const next = event.currentTarget.value; if (next === 'show' || next === 'hide') change({ showCompletedOutput: next === 'show' }); }}><option value="show">Show</option><option value="hide">Hide</option></select></PreferenceRow>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Reading & navigation</legend>
      <PreferenceRow title="Wrap code lines" description="Wrap long code lines in parent and read-only child conversations."><input type="checkbox" aria-label="Wrap code lines" checked={value.wrapCode} onChange={(event) => change({ wrapCode: event.currentTarget.checked })} /></PreferenceRow>
      <PreferenceRow title="Wrap diff lines" description="Wrap supplied inline diffs and the independent Changes inspector."><input type="checkbox" aria-label="Wrap diff lines" checked={value.wrapDiff} onChange={(event) => change({ wrapDiff: event.currentTarget.checked })} /></PreferenceRow>
      <PreferenceRow title="After sending a message" description="Move to your new local message, or stay at your reading position."><select className={controls.select} aria-label="Scroll after local send" value={value.scrollAfterSend ? 'latest' : 'keep'} onChange={(event) => { const next = event.currentTarget.value; if (next === 'latest' || next === 'keep') change({ scrollAfterSend: next === 'latest' }); }}><option value="latest">Scroll to message</option><option value="keep">Keep reading position</option></select></PreferenceRow>
    </fieldset>
    <figure className={styles.sample} aria-label="Live conversation preview"><figcaption>Reading sample · illustrative, not a run</figcaption><p>Review the change without losing your place.</p><pre data-wrap={value.wrapCode} aria-label="Code wrap sample"><code>{'const title = firstMessage.trim().replace(/\\s+/gu, " "); // a small, local naming preference'}</code></pre>
      <details key={`${value.toolDisplay}:running-sample`} open={toolDetailsOpen(value.toolDisplay, 'running')}><summary>Running tool · sample details</summary><p>Reading the example note… Auto folds this detail when finished, unless you inspect it.</p></details>
      <details key={`${value.toolDisplay}:completed-sample`} open={toolDetailsOpen(value.toolDisplay, 'completed')}><summary>Completed tool · sample details</summary>{value.showCompletedOutput ? <p>Found two headings in the example note.</p> : <p>Completed output hidden. Status and file changes are retained.</p>}<pre data-wrap={value.wrapDiff} aria-label="Diff wrap sample"><code>{'+ Keep this long supplied diff line readable without changing the underlying content or its attribution.'}</code></pre></details>
      <details key={`${value.toolDisplay}:failed-sample`} open={toolDetailsOpen(value.toolDisplay, 'failed')}><summary>Failed tool · sample details</summary><p>Example note unavailable. Auto keeps supplied failures open.</p></details>
    </figure>
    <div className={styles.reset}><p>Restore conversation behavior only.</p><Button disabled={!active || !changed} onClick={() => { if (changed && allowed()) update({ conversation: defaultConversation }); }}>Reset Conversation</Button></div>
  </div>;
}
