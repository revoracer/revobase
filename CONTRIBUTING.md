# Contributing

Thanks for improving `revobase`.

## Development Setup

Initialize dependencies and run the test suite before opening a pull request:

```bash
git submodule update --init --recursive
./build.sh test
```

Use `./build.sh doctor` if local toolchain details need to be checked.

## Pull Requests

- Keep changes focused on one behavior or build concern.
- Add or update tests when behavior changes.
- Preserve the public namespace and include layout unless the API change is
  intentional.
- Run `./build.sh test` and include relevant failures if tests cannot be run.

The API is still early and may change, but changes should remain explicit and
reviewable.
