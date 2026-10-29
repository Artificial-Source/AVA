import { type ChildProcessWithoutNullStreams, spawn } from 'node:child_process'
import { randomUUID } from 'node:crypto'
import { IPC_CHANNELS, validateChannelPayload } from '../shared/ipc-contract.ts'
import { resolveLaunchPaths, validateWorkspaceDirectory } from '../shared/launch-config.ts'
import { questionReplySemanticError } from '../shared/question-form.ts'
import { ResolverStore, questionReplyFields } from '../shared/resolver-store.ts'
import { RpcError, RpcSession, type RpcEvent } from '../shared/rpc-session.ts'
import { errorMessage } from '../shared/text.ts'
import {
  applyPromptFinalText,
  applyRpcEvent,
  applyRunError,
  createTranscriptState,
  loadHistoryMessages,
  parseModelsResult,
  parseProtocolResult,
  parseSessionsResult,
  parseStateResult,
  resetTranscript,
  type TranscriptState
} from '../shared/transcript-reducer.ts'
import {
  emptyHostSnapshot,
  type ActionFailure,
  type ActionResult,
  type HostSnapshot,
  type JsonObject,
  type PermissionDecision,
  type QuestionReplyInput,
  type UserFacingError
} from '../shared/types.ts'

export type BackendOptions = {
  packageRoot: string
  cwd: string
  env: NodeJS.ProcessEnv
  // Main owns the native folder dialog; resolves null when the user cancels it.
  chooseWorkspace: () => Promise<string | null>
  handshakeTimeoutMs?: number
}

// A binary that never answers get_protocol (wrong executable, wedged startup) must not leave the
// UI in "starting" forever; Restart is disabled until the connection settles.
const DEFAULT_HANDSHAKE_TIMEOUT_MS = 30_000
// After stdin EOF AVA cancels active work and exits; escalate only if it does not.
const CHILD_EOF_GRACE_MS = 1500
const CHILD_TERM_GRACE_MS = 1500

function rpcUserError(error: unknown, fallbackTitle: string): UserFacingError {
  if (error instanceof RpcError) {
    const hint =
      error.code === 'permission_denied' || /auth|credential|api key|offline/i.test(`${error.message} ${error.details}`)
        ? 'Configure provider credentials with `ava connect` in the terminal. This desktop client has no auth UI.'
        : error.code === 'active_run'
          ? 'Wait for the current run to finish, or cancel it.'
          : 'See docs/desktop/README.md and docs/rpc-protocol.md.'
    return {
      title: fallbackTitle,
      message: error.message,
      hint
    }
  }
  return {
    title: fallbackTitle,
    message: errorMessage(error),
    hint: 'If the AVA binary is missing, build it or set AVA_DESKTOP_BINARY. See docs/desktop/README.md.'
  }
}

function invalidRequest(reason: string): ActionFailure {
  return { ok: false, error: { title: 'Invalid request', message: reason, hint: 'The desktop IPC surface is fixed and narrow.' } }
}

const SHUTTING_DOWN: ActionFailure = {
  ok: false,
  error: { title: 'Shutting down', message: 'The desktop client is closing AVA.', hint: '' }
}

function generateRequestId(prefix: string): string {
  return `${prefix}-${randomUUID()}`
}

function hasExited(child: ChildProcessWithoutNullStreams): boolean {
  return child.exitCode !== null || child.signalCode !== null
}

// Resolves true once the child has exited, or false after timeoutMs.
function waitForExit(child: ChildProcessWithoutNullStreams, timeoutMs: number): Promise<boolean> {
  if (hasExited(child)) {
    return Promise.resolve(true)
  }
  return new Promise((resolve) => {
    const onExit = () => {
      clearTimeout(timer)
      resolve(true)
    }
    const timer = setTimeout(() => {
      child.off('exit', onExit)
      resolve(false)
    }, timeoutMs)
    child.once('exit', onExit)
  })
}

// Ends stdin (AVA's documented disconnect), then escalates to SIGTERM and SIGKILL so a wedged
// backend never outlives the desktop client.
async function closeChild(child: ChildProcessWithoutNullStreams): Promise<void> {
  if (child.stdin.writable) {
    child.stdin.end()
  }
  // No pid: the spawn itself failed, so there is no process to wait for or signal.
  if (child.pid === undefined || await waitForExit(child, CHILD_EOF_GRACE_MS)) {
    return
  }
  child.kill('SIGTERM')
  if (await waitForExit(child, CHILD_TERM_GRACE_MS)) {
    return
  }
  child.kill('SIGKILL')
}

export class AvaBackend {
  readonly #packageRoot: string
  readonly #env: NodeJS.ProcessEnv
  readonly #cwd: string
  readonly #chooseWorkspace: () => Promise<string | null>
  readonly #handshakeTimeoutMs: number
  // AVA_DESKTOP_WORKSPACE, replaced by a workspace picked in the native dialog.
  #workspaceOverride: string | undefined
  #binaryPath = ''
  #workspace = ''
  #child: ChildProcessWithoutNullStreams | null = null
  #rpc: RpcSession | null = null
  #resolvers = new ResolverStore(randomUUID)
  #transcript: TranscriptState = createTranscriptState()
  #snapshot: HostSnapshot
  #listeners = new Set<(snapshot: HostSnapshot) => void>()
  #busy = false
  #canceling = false
  // The single in-flight start or stop-and-start transaction. It is claimed synchronously,
  // before any await: overlapping transactions would spawn a second AVA and orphan the first.
  #lifecycle: Promise<void> | null = null
  // Set by stop() on application quit; no AVA is spawned afterwards.
  #disposed = false
  // Replaced children still being closed (EOF, SIGTERM, SIGKILL); stop() waits for them.
  #exiting = new Set<Promise<void>>()
  #runGeneration = 0

  constructor(options: BackendOptions) {
    this.#packageRoot = options.packageRoot
    this.#cwd = options.cwd
    this.#env = options.env
    this.#chooseWorkspace = options.chooseWorkspace
    this.#handshakeTimeoutMs = options.handshakeTimeoutMs ?? DEFAULT_HANDSHAKE_TIMEOUT_MS
    this.#workspaceOverride = options.env.AVA_DESKTOP_WORKSPACE
    this.#snapshot = this.#emptySnapshot('starting', null)
  }

  snapshot(): HostSnapshot {
    return this.#snapshot
  }

  subscribe(listener: (snapshot: HostSnapshot) => void): () => void {
    this.#listeners.add(listener)
    listener(this.#snapshot)
    return () => {
      this.#listeners.delete(listener)
    }
  }

  // Joins an in-flight start or restart instead of launching a second AVA.
  start(): Promise<void> {
    return this.#lifecycle ?? this.#runLifecycle(() => this.#launch())
  }

  // Terminal (application quit): drops the live AVA, lets an in-flight restart settle without
  // spawning, and waits until every child this backend started has exited.
  async stop(reason = 'AVA RPC stopped'): Promise<void> {
    this.#disposed = true
    await this.#shutdown(reason, 'stopped')
    await this.#lifecycle
    await Promise.allSettled(this.#exiting)
  }

  async handle(channel: string, payload: unknown): Promise<HostSnapshot | ActionResult> {
    const request = validateChannelPayload(channel, payload)
    if (!request.ok) {
      return invalidRequest(request.reason)
    }
    switch (request.channel) {
      case IPC_CHANNELS.getSnapshot:
        return this.#snapshot
      case IPC_CHANNELS.selectWorkspace:
        return this.#selectWorkspace()
      case IPC_CHANNELS.restart:
        return this.#restart()
      case IPC_CHANNELS.newSession:
        return this.#newSession()
      case IPC_CHANNELS.openSession:
        return this.#openSession(request.sessionId)
      case IPC_CHANNELS.setModel:
        return this.#setModel(request.model, request.provider)
      case IPC_CHANNELS.prompt:
        return this.#prompt(request.message)
      case IPC_CHANNELS.cancel:
        return this.#cancel()
      case IPC_CHANNELS.replyPermission:
        return this.#replyPermission(request.displayToken, request.decision)
      case IPC_CHANNELS.replyQuestion:
        return this.#replyQuestion(request.displayToken, request.reply)
    }
  }

  async #spawnAndHandshake(): Promise<void> {
    const resolved = resolveLaunchPaths({
      envBinary: this.#env.AVA_DESKTOP_BINARY,
      envWorkspace: this.#workspaceOverride,
      packageRoot: this.#packageRoot,
      cwd: this.#cwd,
      requireExistingBinary: true
    })
    if (!resolved.ok) {
      this.#binaryPath = this.#env.AVA_DESKTOP_BINARY ?? ''
      this.#workspace = this.#workspaceOverride || this.#cwd
      this.#publish(this.#emptySnapshot('error', resolved.error))
      return
    }
    this.#binaryPath = resolved.value.binaryPath
    this.#workspace = resolved.value.workspace

    let child: ChildProcessWithoutNullStreams
    try {
      child = spawn(this.#binaryPath, ['--rpc'], {
        cwd: this.#workspace,
        env: this.#env,
        stdio: ['pipe', 'pipe', 'pipe'],
        shell: false,
        windowsHide: true
      })
    } catch (error) {
      this.#publish(
        this.#emptySnapshot('error', {
          title: 'Failed to start AVA',
          message: errorMessage(error),
          hint: 'Confirm AVA_DESKTOP_BINARY is a Linux ava executable. See docs/desktop/README.md.'
        })
      )
      return
    }

    this.#child = child
    child.stderr.on('data', () => {
      /* Drain stderr. Do not log contents (prompts, secrets, authority). */
    })
    child.stderr.on('error', () => {
      /* Stderr is diagnostics only; losing it must not crash main. */
    })
    const rpc = new RpcSession({
      write: (bytes) => {
        if (!child.stdin.writable) {
          throw new Error('AVA RPC stdin is closed')
        }
        child.stdin.write(bytes, (error) => {
          if (error) {
            rpc.fail(error)
          }
        })
      },
      onEvent: (event) => {
        if (this.#rpc === rpc) {
          this.#onEvent(event)
        }
      },
      onFailure: (error) => {
        if (this.#rpc === rpc) {
          this.#onProcessFailure(rpcUserError(error, 'AVA RPC disconnected'))
        }
      },
      generateId: generateRequestId
    })
    this.#rpc = rpc
    child.stdin.on('error', (error) => rpc.fail(error))
    child.stdout.on('error', (error) => rpc.fail(error))
    child.stdout.on('data', (chunk: Buffer) => {
      rpc.handleStdout(chunk)
    })
    child.stdout.on('end', () => {
      rpc.handleStdoutEnd()
    })
    child.on('error', (error) => {
      if (this.#child === child) {
        this.#onProcessFailure(rpcUserError(error, 'AVA process error'))
      }
    })
    child.on('exit', (code, signal) => {
      if (this.#child !== child) {
        return
      }
      const detail = signal ? `signal ${signal}` : `exit ${code ?? 'unknown'}`
      this.#onProcessFailure({
        title: 'AVA stopped',
        message: `The backend process ended (${detail}).`,
        hint: 'Restart from the header when idle, or inspect the binary path. Stderr is not shown because it may contain secrets.'
      })
    })

    const handshakeTimer = setTimeout(() => {
      rpc.fail(new Error(`AVA did not answer the RPC handshake within ${this.#handshakeTimeoutMs} ms.`))
    }, this.#handshakeTimeoutMs)
    try {
      const protocol = parseProtocolResult(await rpc.request('get_protocol'))
      const state = parseStateResult(await rpc.request('get_state'))
      const [modelsResult, sessionsResult, messagesResult] = await Promise.all([
        rpc.request('list_models'),
        rpc.request('list_sessions'),
        rpc.request('get_messages')
      ])
      this.#transcript = loadHistoryMessages(createTranscriptState(), messagesResult.messages)
      this.#publish({
        connection: 'ready',
        connectionError: null,
        lastActionError: null,
        protocol,
        state,
        binaryPath: this.#binaryPath,
        workspace: state.workspaceDir || this.#workspace,
        sessions: parseSessionsResult(sessionsResult),
        models: parseModelsResult(modelsResult),
        items: this.#transcript.items,
        busy: false,
        canceling: false,
        pendingPermission: null,
        pendingQuestion: null
      })
    } catch (error) {
      await this.#shutdown('handshake failed', 'error')
      this.#publish(this.#emptySnapshot('error', rpcUserError(error, 'AVA RPC handshake failed')))
    } finally {
      clearTimeout(handshakeTimer)
    }
  }

  #onEvent(event: RpcEvent): void {
    if (!this.#canceling) {
      this.#resolvers.bindEvent(event)
    }
    this.#transcript = applyRpcEvent(this.#transcript, event)
    this.#publish({
      ...this.#snapshot,
      items: this.#transcript.items,
      pendingPermission: this.#resolvers.pendingPermission,
      pendingQuestion: this.#resolvers.pendingQuestion,
      busy: this.#busy,
      canceling: this.#canceling
    })
  }

  #onProcessFailure(error: UserFacingError): void {
    this.#runGeneration += 1
    this.#resolvers.clear()
    this.#busy = false
    this.#canceling = false
    const rpc = this.#rpc
    const child = this.#child
    this.#rpc = null
    this.#child = null
    rpc?.fail(new Error(error.message))
    // EOF/write failures may leave a live child; close it without retaining authority.
    if (child && !hasExited(child)) {
      void this.#retire(child)
    }
    this.#publish({
      ...this.#snapshot,
      connection: 'error',
      connectionError: error,
      busy: false,
      canceling: false,
      pendingPermission: null,
      pendingQuestion: null
    })
  }

  async #refreshCatalog(): Promise<void> {
    const rpc = this.#requireRpc()
    const [stateResult, modelsResult, sessionsResult] = await Promise.all([
      rpc.request('get_state'),
      rpc.request('list_models'),
      rpc.request('list_sessions')
    ])
    const state = parseStateResult(stateResult)
    this.#publish({
      ...this.#snapshot,
      state,
      workspace: state.workspaceDir || this.#workspace,
      sessions: parseSessionsResult(sessionsResult),
      models: parseModelsResult(modelsResult)
    })
  }

  async #refreshMessages(): Promise<void> {
    const rpc = this.#requireRpc()
    const messages = await rpc.request('get_messages')
    this.#transcript = loadHistoryMessages(this.#transcript, messages.messages)
    this.#publish({ ...this.#snapshot, items: this.#transcript.items })
  }

  #guardIdle(): ActionFailure | null {
    if (this.#snapshot.connection !== 'ready' || !this.#rpc) {
      return {
        ok: false,
        error: this.#snapshot.connectionError ?? {
          title: 'Backend not ready',
          message: 'AVA RPC is not connected.',
          hint: 'Fix the binary/workspace error, then restart.'
        }
      }
    }
    if (this.#busy) {
      return {
        ok: false,
        error: {
          title: 'Run in progress',
          message: 'Session, model, workspace, and restart stay disabled until the prompt response settles.',
          hint: 'Cancel the run if you need to change session or model.'
        }
      }
    }
    return null
  }

  async #selectWorkspace(): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    const workspace = await this.#chooseWorkspace()
    if (workspace === null) {
      return { ok: true }
    }
    const invalid = validateWorkspaceDirectory(workspace)
    if (invalid) {
      return { ok: false, error: invalid }
    }
    // A run may have started while the dialog was open; never switch workspace under it.
    const busy = this.#guardIdle()
    if (busy) {
      return busy
    }
    return this.#restart(workspace)
  }

  // `workspace` replaces the launch workspace, but only once this restart owns the lifecycle.
  async #restart(workspace?: string): Promise<ActionResult> {
    if (this.#disposed) {
      return SHUTTING_DOWN
    }
    if (this.#lifecycle) {
      return { ok: false, error: { title: 'Restart in progress', message: 'AVA is already starting.', hint: '' } }
    }
    const idle = this.#guardIdle()
    if (idle && this.#snapshot.connection === 'ready') {
      return idle
    }
    if (workspace !== undefined) {
      this.#workspaceOverride = workspace
    }
    await this.#runLifecycle(async () => {
      // Publishes 'starting' before the first await, so Restart stays disabled throughout.
      await this.#shutdown('restart', 'starting')
      this.#transcript = createTranscriptState()
      await this.#launch()
    })
    // stop() ran during the restart, so AVA is closed rather than restarted.
    if (this.#disposed) {
      return SHUTTING_DOWN
    }
    if (this.#snapshot.connection === 'error') {
      return { ok: false, error: this.#snapshot.connectionError ?? { title: 'Restart failed', message: 'Backend did not become ready.', hint: '' } }
    }
    return { ok: true }
  }

  async #newSession(): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    try {
      await this.#requireRpc().request('new_session')
      this.#transcript = resetTranscript(this.#transcript)
      await this.#refreshCatalog()
      await this.#refreshMessages()
      this.#setActionError(null)
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Could not create session')
      this.#setActionError(userError)
      return { ok: false, error: userError }
    }
  }

  async #openSession(sessionId: string): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    try {
      await this.#requireRpc().request('open_session', { session_id: sessionId })
      await this.#refreshCatalog()
      await this.#refreshMessages()
      this.#setActionError(null)
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Could not open session')
      this.#setActionError(userError)
      return { ok: false, error: userError }
    }
  }

  async #setModel(model: string, provider?: string): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    try {
      const payload: JsonObject = { model }
      if (provider) {
        payload.provider = provider
      }
      await this.#requireRpc().request('set_model', payload)
      await this.#refreshCatalog()
      this.#setActionError(null)
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Could not set model')
      this.#setActionError(userError)
      return { ok: false, error: userError }
    }
  }

  async #prompt(message: string): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    const rpc = this.#requireRpc()
    const generation = ++this.#runGeneration
    this.#busy = true
    this.#canceling = false
    this.#setActionError(null)
    this.#publish({ ...this.#snapshot, busy: true, lastActionError: null })
    let result: JsonObject
    try {
      result = await rpc.request('prompt', { message }, 'prompt')
    } catch (error) {
      if (this.#rpc !== rpc || this.#runGeneration !== generation) {
        return { ok: false, error: rpcUserError(error, 'Prompt ended') }
      }
      this.#busy = false
      this.#canceling = false
      this.#resolvers.clear()
      const userError = rpcUserError(error, 'Prompt failed')
      this.#transcript = applyRunError(this.#transcript, userError.message)
      this.#publish({
        ...this.#snapshot,
        items: this.#transcript.items,
        busy: false,
        canceling: false,
        lastActionError: userError,
        pendingPermission: null,
        pendingQuestion: null
      })
      return { ok: false, error: userError }
    }
    if (this.#rpc !== rpc || this.#runGeneration !== generation) {
      return { ok: false, error: { title: 'Run ended', message: 'This run is no longer current.', hint: '' } }
    }
    const finalText = typeof result.final_text === 'string' ? result.final_text : ''
    this.#transcript = applyPromptFinalText(this.#transcript, finalText)
    this.#busy = false
    this.#canceling = false
    this.#resolvers.clear()
    this.#publish({
      ...this.#snapshot,
      items: this.#transcript.items,
      busy: false,
      canceling: false,
      pendingPermission: null,
      pendingQuestion: null
    })
    // The run succeeded; a failed refresh only leaves the session/model lists stale.
    try {
      await this.#refreshCatalog()
    } catch (error) {
      if (this.#rpc === rpc) {
        this.#setActionError(rpcUserError(error, 'Could not refresh sessions and models'))
      }
    }
    return { ok: true }
  }

  async #cancel(): Promise<ActionResult> {
    if (!this.#rpc || this.#snapshot.connection !== 'ready') {
      return {
        ok: false,
        error: { title: 'Nothing to cancel', message: 'The backend is not running a prompt.', hint: '' }
      }
    }
    // The run already settled (e.g. it finished while the click was in flight). Sending cancel
    // now would latch `canceling` with no run left to clear it.
    if (!this.#busy) {
      return { ok: true }
    }
    if (this.#canceling) {
      return { ok: false, error: { title: 'Cancel pending', message: 'Cancellation is already in progress.', hint: '' } }
    }
    const rpc = this.#rpc
    const generation = this.#runGeneration
    this.#canceling = true
    this.#resolvers.clear()
    this.#publish({ ...this.#snapshot, canceling: true, pendingPermission: null, pendingQuestion: null })
    try {
      await rpc.request('cancel')
      if (this.#rpc !== rpc || this.#runGeneration !== generation) {
        return { ok: true }
      }
      this.#resolvers.clear()
      this.#publish({
        ...this.#snapshot,
        pendingPermission: null,
        pendingQuestion: null
      })
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Cancel failed')
      if (this.#rpc === rpc && this.#runGeneration === generation) {
        this.#canceling = false
        this.#publish({ ...this.#snapshot, canceling: false, lastActionError: userError })
      }
      return { ok: false, error: userError }
    }
  }

  async #replyPermission(displayToken: string, decision: PermissionDecision): Promise<ActionResult> {
    const rpc = this.#rpc
    const pending = this.#resolvers.beginPermission(displayToken)
    if (!pending || !rpc) {
      return {
        ok: false,
        error: { title: 'No permission request', message: 'This permission is no longer displayed or is already being answered.', hint: '' }
      }
    }
    try {
      await rpc.request('permission_reply', {
        request_id: pending.resolverRequestId,
        correlation_id: pending.correlationId,
        decision
      })
      if (this.#rpc === rpc && this.#resolvers.finishPermission(pending)) {
        this.#publish({ ...this.#snapshot, pendingPermission: this.#resolvers.pendingPermission })
      }
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Permission reply failed')
      // The backend's answer is unknown and cannot be retried; retire only the matched
      // display, then fail the same run closed so no resolver stays live on the backend.
      if (this.#rpc === rpc && this.#resolvers.retirePermission(pending)) {
        void this.#cancel()
        this.#setActionError(userError)
      }
      return { ok: false, error: userError }
    }
  }

  async #replyQuestion(displayToken: string, reply: QuestionReplyInput): Promise<ActionResult> {
    const rpc = this.#rpc
    const current = this.#resolvers.peekQuestion(displayToken)
    if (current && rpc) {
      const invalid = questionReplySemanticError(current.view, reply)
      if (invalid) {
        return {
          ok: false,
          error: {
            title: 'Invalid question reply',
            message: invalid,
            hint: 'Correct the answer for the displayed question; nothing was sent and it stays open.'
          }
        }
      }
    }
    const pending = this.#resolvers.beginQuestion(displayToken)
    if (!pending || !rpc) {
      return {
        ok: false,
        error: { title: 'No question', message: 'This question is no longer displayed or is already being answered.', hint: '' }
      }
    }
    try {
      await rpc.request('question_reply', {
        request_id: pending.resolverRequestId,
        correlation_id: pending.correlationId,
        ...questionReplyFields(reply)
      })
      if (this.#rpc === rpc && this.#resolvers.finishQuestion(pending)) {
        this.#publish({ ...this.#snapshot, pendingQuestion: this.#resolvers.pendingQuestion })
      }
      return { ok: true }
    } catch (error) {
      const userError = rpcUserError(error, 'Question reply failed')
      // The backend's answer is unknown and cannot be retried; retire only the matched
      // display, then fail the same run closed so no resolver stays live on the backend.
      if (this.#rpc === rpc && this.#resolvers.retireQuestion(pending)) {
        void this.#cancel()
        this.#setActionError(userError)
      }
      return { ok: false, error: userError }
    }
  }

  #requireRpc(): RpcSession {
    if (!this.#rpc) {
      throw new Error('AVA RPC is not connected')
    }
    return this.#rpc
  }

  #setActionError(error: UserFacingError | null): void {
    this.#publish({ ...this.#snapshot, lastActionError: error })
  }

  #emptySnapshot(connection: HostSnapshot['connection'], error: UserFacingError | null): HostSnapshot {
    return emptyHostSnapshot(connection, error, this.#binaryPath, this.#workspace)
  }

  #publish(snapshot: HostSnapshot): void {
    this.#snapshot = snapshot
    for (const listener of this.#listeners) {
      listener(snapshot)
    }
  }

  #runLifecycle(transaction: () => Promise<void>): Promise<void> {
    const running = transaction().finally(() => {
      this.#lifecycle = null
    })
    this.#lifecycle = running
    return running
  }

  async #launch(): Promise<void> {
    if (this.#disposed) {
      return
    }
    this.#publish(this.#emptySnapshot('starting', null))
    await this.#spawnAndHandshake()
  }

  // Revokes the current AVA's authority (RPC, resolvers, run) at once, publishes `connection`,
  // then waits for the process to exit.
  async #shutdown(reason: string, connection: HostSnapshot['connection']): Promise<void> {
    const rpc = this.#rpc
    const child = this.#child
    this.#rpc = null
    this.#child = null
    this.#runGeneration += 1
    this.#resolvers.clear()
    this.#busy = false
    this.#canceling = false
    this.#publish({
      ...this.#snapshot,
      connection,
      busy: false,
      canceling: false,
      pendingPermission: null,
      pendingQuestion: null
    })
    rpc?.fail(new Error(reason))
    if (child) {
      await this.#retire(child)
    }
  }

  #retire(child: ChildProcessWithoutNullStreams): Promise<void> {
    const exiting: Promise<void> = closeChild(child).finally(() => {
      this.#exiting.delete(exiting)
    })
    this.#exiting.add(exiting)
    return exiting
  }
}
