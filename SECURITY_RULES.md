# SECURITY_RULES.md — Mandatory for all agents

## Rules (non-negotiable)

1. NEVER commit .env, .env.local, or files containing API keys, tokens, or passwords
2. NEVER invent seed data. If no real data exists, leave empty
3. NEVER deploy to production without explicit approval from Ibrahim
4. NEVER hardcode credentials in code — always use environment variables
5. NEVER push without verifying the build compiles clean
6. All .env files must be in .gitignore BEFORE the first commit of the project
7. If an exposed key is detected in history, report IMMEDIATELY
8. Test data must be clearly marked as TEST and never mixed with production

## Enforcement

These rules apply to ALL agents: Claude Code, Antigravity, and any other automated agent.
Violations must be flagged immediately and rolled back.
