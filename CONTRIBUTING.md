# Contributing to LIMA

Thanks for your interest in contributing. LIMA is an AGPLv3 open-source project — improvements stay open.

---

## Reporting Issues

Please use [GitHub Issues](https://github.com/jknoxdev/lima-node/issues) for bugs, enhancements, and feature requests.

- Use the appropriate label: `bug`, `enhancement`, or `roadmap`
- For security vulnerabilities, **do not open a public issue** — see [SECURITY.md](SECURITY.md)

### A note on terminology

If you see the term *graduate* bugs that's just something I made up rather than squash them. If you see issues labeled `graduation` or commit messages referencing a graduating class (e.g. `class of 26w14`), that's the project's way of saying a bug got the sendoff it deserved. 🎓

---

## Making Changes

### Branch convention

```
feat/<short-description>     # new functionality
fix/<short-description>      # bug fixes
docs/<short-description>     # documentation only
refactor/<short-description> # no behaviour change
```

For small fixes (typos, one-liners, comment corrections), committing directly to `main` is fine.

### Commit messages

Please follow [Conventional Commits](https://www.conventionalcommits.org/):

```
feat(firmware): add NVS persistence for sequence counter
fix(gateway): remove hardcoded NODE_MAC — load from config
docs(client): correct misleading comment in reconstruct_lf
```

Reference issues in the commit body with `Fixes #N` — GitHub will auto-close the issue on merge to main.

### Pull requests

- Keep PRs focused — one logical change per PR
- Squash merge is preferred for clean history
- Delete the branch after merge

---

## Code Style

### Firmware (C / Zephyr)

- codebase is currently following Zephyr RTOS conventions
- `LOG_INF` / `LOG_ERR` for all diagnostic output — no `printf`
- `BUILD_ASSERT` for wire format size enforcement
- hardware stubs currently marked with `[STUB]` in the log string

### Gateway / Client (Rust)

- `cargo fmt` before committing
- `cargo clippy` — no warnings
- Keep `lima-types` as the single source of truth for wire format constants
- No `unwrap()` in production paths — use `?` or explicit error handling

---

## Architecture Decisions

Significant design decisions are documented as ADRs in [`docs/architecture/adr/`](docs/architecture/adr/). If your contribution changes a major design assumption, add or update an ADR.

---

## License

By contributing, you agree that your contributions will be licensed under the [AGPLv3](LICENSE). Commercial use outside AGPLv3 terms requires a separate license — see [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md).