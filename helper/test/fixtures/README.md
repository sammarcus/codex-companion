# Fixtures

Two rollout files. `docs/codex-state-format.md` section 8 is the authority on
where each came from; this note exists only to answer the two questions
somebody will ask when a text scan flags this directory.

## `session-sample.jsonl` is a real capture, with two redactions

It is a verbatim capture of a real Codex CLI 0.153.4 session, kept because a
hand-written file cannot prove what a real one does. Two things were taken out
of it before publishing, and nothing else was touched:

- The home directory of the machine that recorded it now reads `/Users/you`.
- The injected skills listing (`payload.content[0].text` on line 3, and
  `payload.state.host_skills.body` on line 5) keeps one entry as a shape
  example, in place of that machine's installed skills. A bracketed note in
  the file itself says so.

No line was added or removed, no line kind changed, and no number changed. The
project's headline claim is that the helper reads counters and never session
content, so a fixture that inventoried somebody's tooling was the wrong thing
to ship next to it.

## It also contains six U+2014 characters, on purpose

The project rule is no em dashes anywhere (`CONTRIBUTING.md`), and this is the
one file in the tree that breaks it. It is not authored prose. All six are on
line 1, inside `payload.base_instructions.text`, which is text Codex itself
wrote. Editing them would make the file a paraphrase of a session rather than a
recording of one.

A scan that walks the whole tree should exempt this directory deliberately
rather than be surprised by it. Search for the single character U+2014, and
skip `.git`, `.pio`, `node_modules`, `vendor` and this `fixtures` directory.

The same reasoning does not extend to `session-sample-synthetic.jsonl`, which
is hand assembled and therefore authored: the rule applies to it in full.
