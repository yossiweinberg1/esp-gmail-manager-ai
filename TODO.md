# AI Relay Upgrade TODO

## 1. PSRAM response cache + local intent filter
- [x] Add a fixed-size PSRAM response cache with 16-32 entries.
- [x] Build cache keys from normalized user message + last 1-2 turns, or a compact prompt fingerprint.
- [x] Return cached replies immediately on hit and log cache hits.
- [x] Add a lightweight local intent filter before Gemini calls.
- [x] Short-circuit pure admin commands, acknowledgements, rate-limit cases, and muted cases.
- [x] Only call Gemini for real conversational turns that miss the cache.

## 2. Smarter context packing for Gemini
- [x] Keep large PSRAM-backed history storage.
- [x] Include the system persona in every prompt.
- [x] Include relevant memory and knowledge snippets.
- [x] Include only the most recent 4-8 turns in full for normal users.
- [x] Preserve full prompt access for admins where appropriate.
- [ ] Optionally add a short rolling summary of older turns per conversation.
- [x] Keep prompts within token limits and reduce Gemini cost/latency.

## 3. Robustness polish
- [x] Increase `uiTask` stack to 6-8 KB if it is still 4 KB.
- [x] Ensure both `networkTask` and `uiTask` are registered with the task watchdog.
- [x] Confirm IMAP stays connected across successful polls and only disconnects on pause, error, or idle TTL.
- [x] Add free heap, free PSRAM, and cache-hit stats to the paused status pages.
- [x] Add the same stats to `!STATUS` if it is easy.

## 4. Optional but welcome
- [x] Add cache statistics such as hits, misses, and size.
- [x] Expose cache statistics on a status page or via `!KEYS` / `!STATUS`.
- [ ] Remove any unused admin rate-limit counters or dead helper functions.

## Notes
- Priority order should stay centered on reducing Gemini API calls first.
- Keep the network path lean so the dual-core split remains stable.
- Prefer small, testable changes for each item.
