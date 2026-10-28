import type { QuestionPromptView, QuestionReplyInput } from './types.ts'

export type QuestionFormState = {
  displayToken: string
  selected: string[]
  custom: string
}

export function createQuestionFormState(displayToken: string): QuestionFormState {
  return { displayToken, selected: [], custom: '' }
}

export function selectQuestionOption(
  state: QuestionFormState,
  question: QuestionPromptView,
  value: string
): QuestionFormState {
  if (question.multiple) {
    return {
      ...state,
      selected: state.selected.includes(value)
        ? state.selected.filter((entry) => entry !== value)
        : [...state.selected, value]
    }
  }
  return { ...state, selected: [value], custom: '' }
}

export function setQuestionCustom(
  state: QuestionFormState,
  question: QuestionPromptView,
  custom: string
): QuestionFormState {
  return { ...state, custom, selected: question.multiple ? state.selected : [] }
}

// Mirrors the backend contract for a bound prompt: reject locally what the RPC would reject,
// so a bad answer keeps the same display open instead of being sent and losing the run.
export function questionReplySemanticError(view: QuestionPromptView, reply: QuestionReplyInput): string | null {
  if (reply.selected !== undefined && reply.selectedOptions !== undefined) {
    return 'selected and selectedOptions are mutually exclusive'
  }
  const hasAnswer = reply.answer !== undefined
  if (hasAnswer && !view.allowCustom) {
    return 'answer is not allowed for this question'
  }
  const hasSelected = reply.selected !== undefined
  const hasSelectedOptions = reply.selectedOptions !== undefined
  const submitted = reply.selected !== undefined ? [reply.selected] : (reply.selectedOptions ?? [])
  if (hasSelected || hasSelectedOptions) {
    if (!view.multiple && submitted.length !== 1) {
      return 'single-select questions require exactly one selected option'
    }
    if (!view.multiple && hasAnswer) {
      return 'single-select questions accept an answer or an option, not both'
    }
    if (submitted.length > view.options.length) {
      return 'selectedOptions has more values than the question offers'
    }
    const seen = new Set<string>()
    for (const value of submitted) {
      if (!view.options.some((option) => option.value === value)) {
        return 'selected value is not an option for this question'
      }
      if (seen.has(value)) {
        return 'selectedOptions contains duplicate values'
      }
      seen.add(value)
    }
  }
  if (!hasAnswer && !hasSelected && !hasSelectedOptions) {
    return 'question reply needs an answer, selected value, or selectedOptions'
  }
  return null
}

export function serializeQuestionForm(
  state: QuestionFormState,
  question: QuestionPromptView
): QuestionReplyInput | null {
  if (state.displayToken !== question.displayToken) {
    return null
  }
  const custom = question.allowCustom && (question.secret ? state.custom.length > 0 : Boolean(state.custom.trim()))
  if (question.multiple) {
    if (state.selected.length === 0 && !custom) {
      return null
    }
    return { selectedOptions: state.selected, ...(custom ? { answer: state.custom } : {}) }
  }
  if (custom) {
    return { answer: state.custom }
  }
  return state.selected.length > 0 ? { selected: state.selected[0] } : null
}
