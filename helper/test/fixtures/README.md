# Fixtures

Two rollout files. `docs/codex-state-format.md` section 8 is the authority on
where each came from; this note exists only to answer the question somebody
will ask when a text scan flags this directory.

## `session-sample.jsonl` contains three U+2014 characters, on purpose

The project rule is no em dashes anywhere (`CONTRIBUTING.md`), and this is the
one file in the tree that breaks it. It is not authored prose. It is a verbatim
capture of a real Codex CLI 0.153.4 session, and all three occurrences are
inside data that Codex itself wrote:

- line 1, `payload.base_instructions.text`
- lines 3 and 5, the injected skills listing

Editing them would make the file a paraphrase of a session rather than a
recording of one, and every claim `docs/codex-state-format.md` makes about it
would stop being checkable. So the bytes stay exactly as captured.

A scan that walks the whole tree should exempt this directory deliberately
rather than be surprised by it. Search for the single character U+2014, and
skip `.git`, `.pio`, `node_modules`, `vendor` and this `fixtures` directory.

The same reasoning does not extend to `session-sample-synthetic.jsonl`, which
is hand assembled and therefore authored: the rule applies to it in full.
