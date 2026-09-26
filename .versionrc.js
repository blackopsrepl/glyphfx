// commit-and-tag-version configuration. VERSION is the single version surface;
// the changelog is generated from conventional commits and must never be
// hand-edited.
module.exports = {
  tagPrefix: 'v',
  releaseCommitMessageFormat: 'chore(release): {{currentTag}}',
  packageFiles: [{ filename: 'VERSION', type: 'plain-text' }],
  bumpFiles: [{ filename: 'VERSION', type: 'plain-text' }],
  commitUrlFormat: 'https://github.com/blackopsrepl/glyphfx/commit/{{hash}}',
  compareUrlFormat: 'https://github.com/blackopsrepl/glyphfx/compare/{{previousTag}}...{{currentTag}}',
  issueUrlFormat: 'https://github.com/blackopsrepl/glyphfx/issues/{{id}}',
  header:
    '# Changelog\n\nAll notable changes to glyphfx are documented here. ' +
    'This file is generated from conventional commits by commit-and-tag-version; do not edit by hand.\n',
  types: [
    { type: 'feat', section: 'Features' },
    { type: 'fix', section: 'Bug Fixes' },
    { type: 'perf', section: 'Performance' },
    { type: 'refactor', section: 'Refactoring' },
    { type: 'test', section: 'Tests' },
    { type: 'build', section: 'Build' },
    { type: 'ci', section: 'CI' },
    { type: 'docs', section: 'Documentation' },
    { type: 'chore', hidden: true },
  ],
};
