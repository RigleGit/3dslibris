# Contributing

Thanks for contributing to `3dslibris`.

## Workflow
- Base branch: `main`
- Optional topic branches: use short descriptive names when needed.
- Keep PRs focused and small when possible.

## Local setup
- Recommended: the project Docker image built from `docker/Dockerfile.cia`.
- Require libctru 2.7.1 or later for compatibility with extended application memory layouts such as Luma's Dev1. Updating Luma or Homebrew Menu alone does not update an existing application binary.
- Required outputs must be reproducible from source.

## Build commands

```bash
make clean
make
```

Or with Docker (recommended for consistency):

```bash
docker build --platform linux/amd64 -f docker/Dockerfile.cia -t 3dslibris-cia .
docker run --rm -v "$(pwd):/project" -w /project \
  --platform linux/amd64 \
  -e DEVKITPRO=/opt/devkitpro -e DEVKITARM=/opt/devkitpro/devkitARM \
  3dslibris-cia \
  sh -lc 'make clean && make -j2 && make zip-sdmc && make debug-3dsx'
```

## Code guidelines
- Keep compatibility with Nintendo 3DS memory/CPU constraints.
- Prefer deterministic behavior and explicit fallbacks over implicit heuristics.
- Preserve attribution in files derived from original `dslibris`.
- Add concise comments only where behavior is non-obvious.

## Testing expectations (smoke)

Before opening a PR, verify:
1. App boots and library loads.
2. Open at least one EPUB and one non-EPUB format.
3. Page turning works (`A/B/L/R`).
4. Settings, index and bookmarks menus open/close correctly.
5. `git status` remains clean from generated artifacts after build.

## Commit style
Use imperative, scoped messages, for example:
- `repo: add CI build and artifact hygiene checks`
- `mobi: reduce initial parse latency with deferred pagination`
- `docs: document current format support and limitations`

## Pull requests
- Describe user-visible behavior changes.
- Include logs/screenshots for parsing, TOC, or UI-touch behavior changes.
- Mention performance impact if touching parsing/pagination paths.

## AI assistance
- AI assisted contributions are allowed, but contributors are responsible for ensuring code quality, correctness, and adherence to project guidelines.
- Do not add AI tools as commit co-authors or in commit messages. The contribution is attributed to the human contributor submitting the PR.
