import { type ChildProcessWithoutNullStreams, spawn } from 'node:child_process'
import { randomUUID } from 'node:crypto'
import { dialog } from 'electron'
import {
  IPC_CHANNELS,
  validateChannelPayload,
  validateOpenSession,
  validatePermissionReply,
  validatePrompt,
  validateQuestionReply,
  validateSetModel
} from '../shared/ipc-contract.ts'
import { resolveLaunchPaths, validateWorkspaceDirectory } from '../shared/launch-config.ts'
import { questionReplySemanticError } from '../shared/question-form.ts'
import { ResolverStore, questionReplyFields } from '../shared/resolver-store.ts'
import { RpcError, RpcSession, type RpcEvent } from '../shared/rpc-session.ts'
import { errorMessage } from '../shared/text.ts'
import {
  applyPromptFinalText,
  applyRpcEvent,
  createTranscriptState,
  loadHistoryMessages,
  parseModelsResult,
  parseProtocolResult,
  parseSessionsResult,
  parseStateResult,
  resetTranscript,
  type TranscriptState
} from '../shared/transcript-reducer.ts'
import type {
  HostSnapshot,
  JsonObject,
  PermissionDecision,
  QuestionReplyInput,
  UserFacingError
} from '../shared/types.ts'

export type BackendOptions = {
  packageRoot: string
  cwd: string
  env: NodeJS.ProcessEnv
}

type ActionResult = { ok: true } | { ok: false; error: UserFacingError }

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

function generateRequestId(prefix: string): string {
  return `${prefix}-${randomUUID()}`
}

export class AvaBackend {
  readonly #packageRoot: string
  readonly #env: NodeJS.ProcessEnv
  #cwd: string
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
  #starting = false
  #runGeneration = 0

  constructor(options: BackendOptions) {
    this.#packageRoot = options.packageRoot
    this.#cwd = options.cwd
    this.#env = options.env
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

  async start(): Promise<void> {
    if (this.#starting) {
      return
    }
    this.#starting = true
    this.#publish(this.#emptySnapshot('starting', null))
    try {
      await this.#spawnAndHandshake()
    } finally {
      this.#starting = false
    }
  }

  async stop(reason = 'AVA RPC stopped'): Promise<void> {
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
      busy: false,
      canceling: false,
      pendingPermission: null,
      pendingQuestion: null
    })
    if (rpc) {
      rpc.fail(new Error(reason))
    }
    if (child) {
      await this.#closeChild(child)
    }
  }

  async handle(channel: string, payload: unknown): Promise<unknown> {
    const checked = validateChannelPayload(channel, payload)
    if (!checked.ok) {
      return { ok: false, error: { title: 'Invalid request', message: checked.reason, hint: 'The desktop IPC surface is fixed and narrow.' } }
    }
    switch (channel) {
      case IPC_CHANNELS.getSnapshot:
        return this.#snapshot
      case IPC_CHANNELS.selectWorkspace:
        return this.#selectWorkspace()
      case IPC_CHANNELS.restart:
        return this.#restart()
      case IPC_CHANNELS.newSession:
        return this.#newSession()
      case IPC_CHANNELS.openSession: {
        const body = validateOpenSession(payload)
        return body.ok ? this.#openSession(body.sessionId) : { ok: false, error: { title: 'Invalid request', message: body.reason, hint: '' } }
      }
      case IPC_CHANNELS.setModel: {
        const body = validateSetModel(payload)
        return body.ok ? this.#setModel(body.model, body.provider) : { ok: false, error: { title: 'Invalid request', message: body.reason, hint: '' } }
      }
      case IPC_CHANNELS.prompt: {
        const body = validatePrompt(payload)
        return body.ok ? this.#prompt(body.message) : { ok: false, error: { title: 'Invalid request', message: body.reason, hint: '' } }
      }
      case IPC_CHANNELS.cancel:
        return this.#cancel()
      case IPC_CHANNELS.replyPermission: {
        const body = validatePermissionReply(payload)
        return body.ok ? this.#replyPermission(body.displayToken, body.decision) : { ok: false, error: { title: 'Invalid request', message: body.reason, hint: '' } }
      }
      case IPC_CHANNELS.replyQuestion: {
        const body = validateQuestionReply(payload)
        return body.ok ? this.#replyQuestion(body.displayToken, body.reply) : { ok: false, error: { title: 'Invalid request', message: body.reason, hint: '' } }
      }
      default:
        return { ok: false, error: { title: 'Invalid request', message: 'channel is not allowlisted', hint: '' } }
    }
  }

  async #spawnAndHandshake(): Promise<void> {
    const resolved = resolveLaunchPaths({
      envBinary: this.#env.AVA_DESKTOP_BINARY,
      envWorkspace: this.#env.AVA_DESKTOP_WORKSPACE,
      packageRoot: this.#packageRoot,
      cwd: this.#cwd,
      requireExistingBinary: true
    })
    if (!resolved.ok) {
      this.#binaryPath = this.#env.AVA_DESKTOP_BINARY ?? ''
      this.#workspace = this.#env.AVA_DESKTOP_WORKSPACE ?? this.#cwd
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
      await this.stop('handshake failed')
      this.#publish(this.#emptySnapshot('error', rpcUserError(error, 'AVA RPC handshake failed')))
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
    if (child && child.exitCode === null && child.signalCode === null) {
      void this.#closeChild(child)
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

  #guardIdle(): ActionResult | null {
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

  async #selectWorkspace(): Promise<ActionResult & { cancelled?: boolean; workspace?: string }> {
    const idle = this.#guardIdle()
    if (idle) {
      return idle
    }
    const picked = await dialog.showOpenDialog({
      title: 'Choose AVA workspace',
      properties: ['openDirectory']
    })
    if (picked.canceled || picked.filePaths.length === 0) {
      return { ok: true, cancelled: true }
    }
    const workspace = picked.filePaths[0]
    const invalid = validateWorkspaceDirectory(workspace)
    if (invalid) {
      return { ok: false, error: invalid }
    }
    this.#env.AVA_DESKTOP_WORKSPACE = workspace
    this.#cwd = workspace
    const restarted = await this.#restart()
    if (!restarted.ok) {
      return restarted
    }
    return { ok: true, workspace }
  }

  async #restart(): Promise<ActionResult> {
    const idle = this.#guardIdle()
    if (idle && this.#snapshot.connection === 'ready') {
      return idle
    }
    await this.stop('restart')
    this.#transcript = createTranscriptState()
    await this.start()
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
    try {
      const result = await rpc.request('prompt', { message }, 'prompt')
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
      await this.#refreshCatalog()
      return { ok: true }
    } catch (error) {
      if (this.#rpc !== rpc || this.#runGeneration !== generation) {
        return { ok: false, error: rpcUserError(error, 'Prompt ended') }
      }
      this.#busy = false
      this.#canceling = false
      this.#resolvers.clear()
      const userError = rpcUserError(error, 'Prompt failed')
      this.#transcript = applyRpcEvent(this.#transcript, {
        name: 'error',
        type: 'error',
        eventId: '',
        schemaVersion: 1,
        requestId: '',
        correlationId: '',
        payload: { message: userError.message },
        raw: {}
      })
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
  }

  async #cancel(): Promise<ActionResult> {
    if (!this.#rpc || this.#snapshot.connection !== 'ready') {
      return {
        ok: false,
        error: { title: 'Nothing to cancel', message: 'The backend is not running a prompt.', hint: '' }
      }
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
    return {
      connection,
      connectionError: error,
      lastActionError: null,
      protocol: null,
      state: null,
      binaryPath: this.#binaryPath,
      workspace: this.#workspace,
      sessions: [],
      models: [],
      items: [],
      busy: false,
      canceling: false,
      pendingPermission: null,
      pendingQuestion: null
    }
  }

  #publish(snapshot: HostSnapshot): void {
    this.#snapshot = snapshot
    for (const listener of this.#listeners) {
      listener(snapshot)
    }
  }

  async #closeChild(child: ChildProcessWithoutNullStreams): Promise<void> {
    try {
      if (child.stdin.writable) {
        child.stdin.end()
      }
    } catch {
      /* already closed */
    }
    const exited = await new Promise<void>((resolve) => {
      const timer = setTimeout(() => {
        if (child.exitCode === null) {
          child.kill('SIGTERM')
        }
        resolve()
      }, 1500)
      child.once('exit', () => {
        clearTimeout(timer)
        resolve()
      })
    })
    void exited
  }
}
