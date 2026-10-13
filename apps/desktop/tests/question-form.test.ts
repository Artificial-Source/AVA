import assert from 'node:assert/strict'
import { test } from 'node:test'
import { createQuestionFormState, questionReplySemanticError, selectQuestionOption, serializeQuestionForm, setQuestionCustom } from '../src/shared/question-form.ts'
import type { QuestionPromptView, QuestionReplyInput } from '../src/shared/types.ts'

const question: QuestionPromptView = {
  displayToken: 'A', header: 'Choose', question: 'Which?',
  options: [{ value: 'a', label: 'A' }, { value: 'b', label: 'B' }],
  multiple: false, allowCustom: true, secret: false
}

test('single choice option -> custom -> option sends exactly one answer field', () => {
  let state = createQuestionFormState(question.displayToken)
  assert.equal(serializeQuestionForm(state, question), null)
  state = selectQuestionOption(state, question, 'a')
  assert.deepEqual(serializeQuestionForm(state, question), { selected: 'a' })
  state = setQuestionCustom(state, question, 'custom')
  assert.deepEqual(state.selected, [])
  assert.deepEqual(serializeQuestionForm(state, question), { answer: 'custom' })
  state = selectQuestionOption(state, question, 'b')
  assert.equal(state.custom, '')
  assert.deepEqual(serializeQuestionForm(state, question), { selected: 'b' })
})

test('multiple choice can combine options and allowed custom text', () => {
  const multiple = { ...question, multiple: true }
  let state = createQuestionFormState(multiple.displayToken)
  state = selectQuestionOption(state, multiple, 'a')
  state = setQuestionCustom(state, multiple, 'custom')
  state = selectQuestionOption(state, multiple, 'b')
  assert.deepEqual(serializeQuestionForm(state, multiple), { selectedOptions: ['a', 'b'], answer: 'custom' })
  state = selectQuestionOption(state, multiple, 'a')
  assert.deepEqual(serializeQuestionForm(state, multiple), { selectedOptions: ['b'], answer: 'custom' })
  assert.deepEqual(serializeQuestionForm(state, { ...multiple, allowCustom: false }), { selectedOptions: ['b'] })
})

test('secret masks input but never grants custom-answer permission', () => {
  for (const multiple of [false, true]) {
    const secret = { ...question, secret: true, allowCustom: false, multiple }
    let state = setQuestionCustom(createQuestionFormState(secret.displayToken), secret, 'secret value')
    assert.equal(serializeQuestionForm(state, secret), null)
    state = selectQuestionOption(state, secret, 'a')
    assert.deepEqual(serializeQuestionForm(state, secret), multiple ? { selectedOptions: ['a'] } : { selected: 'a' })
  }
  const allowed = { ...question, secret: true }
  const state = setQuestionCustom(createQuestionFormState(allowed.displayToken), allowed, ' secret ')
  assert.deepEqual(serializeQuestionForm(state, allowed), { answer: ' secret ' })
})

test('each new display token resets options and secrets; stale state cannot serialize', () => {
  const old = { ...question, secret: true, multiple: true }
  let state = selectQuestionOption(createQuestionFormState(old.displayToken), old, 'a')
  state = setQuestionCustom(state, old, 'secret')
  const next = { ...old, displayToken: 'B' }
  assert.equal(serializeQuestionForm(state, next), null)
  assert.deepEqual(createQuestionFormState(next.displayToken), { displayToken: 'B', selected: [], custom: '' })
  assert.equal(serializeQuestionForm(createQuestionFormState(next.displayToken), next), null)
})

test('semantic validation mirrors the backend contract for the bound view', () => {
  const single = { ...question, allowCustom: true }
  const secretWithoutCustom = { ...question, allowCustom: false, secret: true }
  const multiple = { ...question, multiple: true }
  const open = { ...question, options: [], allowCustom: true }
  const accepted: [QuestionPromptView, QuestionReplyInput][] = [
    [single, { selected: 'a' }],
    [single, { selectedOptions: ['a'] }],
    [single, { answer: 'custom' }],
    [single, { answer: '' }],
    [open, { answer: '' }],
    [secretWithoutCustom, { selected: 'a' }],
    [multiple, { selected: 'a' }],
    [multiple, { selectedOptions: ['a', 'b'] }],
    [multiple, { selectedOptions: [] }],
    [multiple, { selectedOptions: [], answer: '' }],
    [multiple, { selectedOptions: ['a'], answer: 'custom' }]
  ]
  for (const [view, reply] of accepted) {
    assert.equal(questionReplySemanticError(view, reply), null, JSON.stringify(reply))
  }
  const rejected: [QuestionPromptView, QuestionReplyInput][] = [
    [single, { selected: 'a', selectedOptions: ['a'] }],
    [multiple, { selected: 'a', selectedOptions: ['a'] }],
    [secretWithoutCustom, { answer: 'not allowed' }],
    [single, { selectedOptions: [] }],
    [single, { selectedOptions: ['a', 'b'] }],
    [single, { answer: 'custom', selected: 'a' }],
    [single, { answer: 'custom', selectedOptions: ['a'] }],
    [single, { selected: 'missing' }],
    [single, { selectedOptions: ['a', 'missing'] }],
    [single, { selectedOptions: ['a', 'a'] }],
    [single, { selectedOptions: ['a', 'b', 'b'] }],
    [single, { selected: '' }],
    [multiple, {}]
  ]
  for (const [view, reply] of rejected) {
    assert.notEqual(questionReplySemanticError(view, reply), null, JSON.stringify(reply))
  }
})
