import type { Choice, PermissionView, QuestionAnswer, QuestionView, RequestIdentity, RequestState } from './contracts';

export function validIdentity(value: RequestIdentity) {
  return typeof value.id === 'string' && value.id.trim().length > 0 && Number.isSafeInteger(value.version) && value.version >= 0;
}

export function validChoices(options: readonly Choice[]) {
  return Array.isArray(options) && options.every((option) => option && typeof option.id === 'string' && option.id.trim() && typeof option.label === 'string' && option.label.trim().length > 0) && new Set(options.map((option) => option.id)).size === options.length;
}

function validState(state: RequestState) {
  return ['ready', 'pending', 'submitted', 'error', 'stale', 'closed'].includes(state);
}

export function validPermission(request: PermissionView) {
  return validIdentity(request) && request.kind === 'permission' && validState(request.state) && typeof request.sessionAllowed === 'boolean' && Array.isArray(request.decisions) && request.decisions.length > 0 &&
    new Set(request.decisions).size === request.decisions.length && request.decisions.every((decision) => decision === 'allow' || decision === 'deny' || (decision === 'allow_session' && request.sessionAllowed));
}

export function validQuestion(request: QuestionView) {
  return validIdentity(request) && request.kind === 'question' && validState(request.state) && request.placement === 'inline' &&
    ['single', 'multiple'].includes(request.selection) && typeof request.required === 'boolean' && typeof request.searchable === 'boolean' &&
    validChoices(request.options) && request.custom && ['none', 'text', 'secret'].includes(request.custom.kind) &&
    (request.custom.kind === 'none' || (typeof request.custom.label === 'string' && request.custom.label.trim().length > 0)) && (request.options.length > 0 || request.custom.kind !== 'none');
}

export function validAnswer(request: QuestionView, answer: QuestionAnswer) {
  if (!validQuestion(request) || !Array.isArray(answer.selectedIds) || typeof answer.customText !== 'string') return false;
  const ids = answer.selectedIds;
  if (new Set(ids).size !== ids.length || ids.some((id) => !request.options.some((option) => option.id === id))) return false;
  const custom = answer.customText.trim().length > 0;
  if (request.custom.kind === 'none' && answer.customText.length > 0) return false;
  const count = ids.length + Number(custom);
  return (!request.required || count > 0) && (request.selection !== 'single' || count <= 1);
}
