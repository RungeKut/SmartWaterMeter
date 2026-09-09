# Global rules for GigaCode/GigaCod — SmartWaterMeter

## At session start
Read `docs/_index.md` first — it contains the documentation map and update rules.

## Core rules
1. **Read only what you need** — documentation is modular. If you need to understand one module, read only its `docs/modules/<name>.md`. Do not read all docs at once.
2. **Update docs immediately after code changes** — if you add/remove a method, fix a bug, or change behavior, update the corresponding file in `docs/`. Do not postpone.
3. **Update `CHAT_SUMMARY.md`** after every significant change — add a brief entry describing what was done and why.
4. **Consult `docs/pitfalls.md`** when debugging — known issues (watchdog, EEPROM wear, bus rescan delay) are documented there.
5. **Do NOT use `192.168.4.1`** anywhere — the project uses `192.168.0.1` for AP mode.
6. **Commit messages** — use conventional commits: `fix:`, `feat:`, `docs:`, `chore:`, `refactor:`. Include the `why` in the body.
7. **Full reflash sequence** after firmware changes: `pio run --target upload --upload-port COM3 && pio run --target uploadfs --upload-port COM3`
8. **Never commit `src/secrets.h`** — it's in `.gitignore`.