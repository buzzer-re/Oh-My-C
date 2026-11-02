# Add retry logic

Short intro paragraph explaining **why** retries matter for `fetch` calls.

## Context

The HTTP client currently fails on the first transient error.
- Errors: ECONNRESET, 503
- Callers: sync worker, uploader

## Steps

1. Add a `retry()` helper with exponential backoff.
2. Wrap the client calls.
3. Add tests.

### Edge cases

- Max attempts reached
- Non-idempotent requests

## Verification

Run `make test` and check the new cases pass.
