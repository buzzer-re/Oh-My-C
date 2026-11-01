#!/bin/sh
# TD_SETUP hook for the lsp parity cases: private (unshared) language servers so omp does not attach to a
# long-lived broker clangd whose index outlives the per-case work tree.
printf 'lsp:\n  shared: false\n' >> "$1/config.yml"
