// REVIEW: Weapon Editor Merge (Sunrise-Wyndxgo fork)
// Verifies the weapon editor panel is correctly integrated with the Sunrise codebase.

const meta = {
  name: 'review-weapon-editor-merge',
  description: 'Verify weapon editor merge across dimensions, confirm each finding',
  phases: [{ title: 'Review' }, { title: 'Verify' }]
}

const DIMENSIONS = [
  {
    key: 'bugs',
    prompt: 'Review the weapon editor merge for bugs. Check for null dereferences, unbalanced braces, missing error handling, and any logic errors. Report any findings.',
    schema: FINDINGS_SCHEMA
  },
  {
    key: 'perf',
    prompt: 'Review the weapon editor merge for performance issues. Look for redundant work, missing caching, unnecessary allocations, and tight loops. Report any findings.',
    schema: FINDINGS_SCHEMA
  },
  {
    key: 'design',
    prompt: 'Review the weapon editor merge for design concerns. Check for unclear naming, inconsistent API patterns, and violations of existing conventions. Report any findings.',
    schema: FINDINGS_SCHEMA
  }
]

const results = await pipeline(
  DIMENSIONS,
  d => agent(d.prompt, {label: `review:${d.key}`, phase: 'Review', schema: FINDINGS_SCHEMA})
)

const reviewFindings = results.flat().filter(Boolean)

if (reviewFindings.length === 0) {
  return {
    summary: 'No findings',
    findings: [],
    next: 'None — no findings to verify.'
  }
}

const verifyPrompts = reviewFindings.map(f =>
  `Adversarially verify: ${f.title}. ${f.description}. Be skeptical — try to find a counterexample or show why this is not a real issue.`
)

const verifyResults = await parallel(
  verifyPrompts.map(p => () =>
    agent(p, {label: `verify:${f.key}`, phase: 'Verify', schema: VERDICT_SCHEMA})
  )
)

const confirmed = verifyResults.flat().filter(Boolean).filter(f => f.verdict?.isReal)

return {
  summary: 'Confirmed findings',
  findings: confirmed,
  next: confirmed.length > 0
    ? `The following ${confirmed.length} finding(s) are real: ${confirmed.map(f => f.title).join(', ')}`
    : 'None — all findings were false positives.'
}
