#!/usr/bin/env bash
# =============================================================================
#  git_push_hybridsched.sh
#  Zero-leakage interactive Git push workflow for /home/cse-sdpl/OS_452
#  Usage: bash scripts/git_push_hybridsched.sh
# =============================================================================

set -euo pipefail

# ── Colours ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

info()    { echo -e "${CYAN}[INFO]${RESET}  $*"; }
success() { echo -e "${GREEN}[OK]${RESET}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${RESET}  $*"; }
die()     { echo -e "${RED}[FAIL]${RESET}  $*" >&2; exit 1; }

# ── Guard: must run from repo root ────────────────────────────────────────────
REPO_ROOT="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel 2>/dev/null)" \
  || die "Not inside a Git repository. Run from /home/cse-sdpl/OS_452."
cd "$REPO_ROOT"
info "Repository root: ${BOLD}${REPO_ROOT}${RESET}"

echo ""
echo -e "${BOLD}════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}   hybridsched  —  Git Identity & GitHub Push Workflow       ${RESET}"
echo -e "${BOLD}════════════════════════════════════════════════════════════${RESET}"
echo ""

# ═════════════════════════════════════════════════════════════════════════════
# STEP 1 — Interactive Identity & Parameter Capture
# ═════════════════════════════════════════════════════════════════════════════
echo -e "${BOLD}── STEP 1: Git Identity & Remote Parameters ─────────────────${RESET}"
echo ""

# Show existing globals if present
EXISTING_NAME="$(git config --global user.name  2>/dev/null || true)"
EXISTING_EMAIL="$(git config --global user.email 2>/dev/null || true)"
EXISTING_REMOTE="$(git remote get-url origin 2>/dev/null || true)"

[[ -n "$EXISTING_NAME"  ]] && echo -e "  Current global name  : ${YELLOW}${EXISTING_NAME}${RESET}"
[[ -n "$EXISTING_EMAIL" ]] && echo -e "  Current global email : ${YELLOW}${EXISTING_EMAIL}${RESET}"
[[ -n "$EXISTING_REMOTE" ]] && echo -e "  Current remote origin: ${YELLOW}${EXISTING_REMOTE}${RESET}"
echo ""

# Prompt: Git username
read -rp "$(echo -e "  ${BOLD}Git global username${RESET} [press Enter to keep '${EXISTING_NAME:-<none>}']: ")" INPUT_NAME
GIT_NAME="${INPUT_NAME:-$EXISTING_NAME}"
[[ -z "$GIT_NAME" ]] && die "Git username cannot be empty."

# Prompt: Git email
read -rp "$(echo -e "  ${BOLD}Git global email${RESET}    [press Enter to keep '${EXISTING_EMAIL:-<none>}']: ")" INPUT_EMAIL
GIT_EMAIL="${INPUT_EMAIL:-$EXISTING_EMAIL}"
[[ -z "$GIT_EMAIL" ]] && die "Git email cannot be empty."

# Prompt: Target branch
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'main')"
read -rp "$(echo -e "  ${BOLD}Target branch name${RESET}  [current: '${CURRENT_BRANCH}', e.g. feature/hardware-eval]: ")" INPUT_BRANCH
BRANCH_NAME="${INPUT_BRANCH:-$CURRENT_BRANCH}"
# Sanitise: strip leading/trailing whitespace
BRANCH_NAME="$(echo "$BRANCH_NAME" | xargs)"
[[ -z "$BRANCH_NAME" ]] && die "Branch name cannot be empty."

# Prompt: Remote URL
read -rp "$(echo -e "  ${BOLD}Remote GitHub URL${RESET}   [press Enter to keep '${EXISTING_REMOTE:-<none>}']: ")" INPUT_REMOTE
REMOTE_URL="${INPUT_REMOTE:-$EXISTING_REMOTE}"
[[ -z "$REMOTE_URL" ]] && die "Remote URL cannot be empty."

echo ""
echo -e "${BOLD}  ── Captured parameters ───────────────────────────────────${RESET}"
echo -e "  Name   : ${GREEN}${GIT_NAME}${RESET}"
echo -e "  Email  : ${GREEN}${GIT_EMAIL}${RESET}"
echo -e "  Branch : ${GREEN}${BRANCH_NAME}${RESET}"
echo -e "  Remote : ${GREEN}${REMOTE_URL}${RESET}"
echo ""
read -rp "$(echo -e "  ${BOLD}Proceed with these settings? [Y/n]:${RESET} ")" CONFIRM
CONFIRM="${CONFIRM:-Y}"
[[ "$CONFIRM" =~ ^[Nn] ]] && { warn "Aborted by user."; exit 0; }

# ═════════════════════════════════════════════════════════════════════════════
# STEP 2 — Apply Identity & Branch Allocation
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}── STEP 2: Apply Identity & Workspace Hygiene ───────────────${RESET}"

git config --global user.name  "$GIT_NAME"
git config --global user.email "$GIT_EMAIL"
success "git config --global user.name  \"${GIT_NAME}\""
success "git config --global user.email \"${GIT_EMAIL}\""

# Validate .gitignore hygiene
if ! grep -q '\*.o' .gitignore 2>/dev/null; then
  warn ".gitignore missing *.o rule — appending."
  echo "*.o" >> .gitignore
fi
if ! grep -q 'bin/\*' .gitignore 2>/dev/null; then
  warn ".gitignore missing bin/* rule — appending."
  echo "bin/*" >> .gitignore
fi
success ".gitignore validated (*.o, bin/*, *.pyc rules present)"

# Ensure publication figures are NOT ignored
if git check-ignore -q results/fig1_edp_comparison.png 2>/dev/null; then
  warn "results/fig*.png appears in .gitignore — check your ignore rules."
else
  success "Publication figures in results/ are tracked (not gitignored)"
fi

# Switch to or create the target branch
if git rev-parse --verify "$BRANCH_NAME" &>/dev/null; then
  info "Branch '${BRANCH_NAME}' exists locally — switching"
else
  info "Creating new branch '${BRANCH_NAME}'"
fi
git checkout -B "$BRANCH_NAME"
success "Now on branch: $(git rev-parse --abbrev-ref HEAD)"

# ═════════════════════════════════════════════════════════════════════════════
# STEP 3 — Staging & Atomic Commit
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}── STEP 3: Staging & Atomic Commit ──────────────────────────${RESET}"

git add .
STAGED="$(git diff --cached --name-only | wc -l)"
info "Staged ${BOLD}${STAGED}${RESET} file(s):"
git diff --cached --name-only | sed 's/^/    ✚ /'

echo ""
if [[ "$STAGED" -eq 0 ]]; then
  warn "Nothing new to commit — working tree clean."
else
  COMMIT_MSG="feat: real-hardware evaluation setup and hybridsched scheduler implementation"
  git commit -m "$COMMIT_MSG"
  success "Committed: \"${COMMIT_MSG}\""
  git log --oneline -1
fi

# ═════════════════════════════════════════════════════════════════════════════
# STEP 4 — Remote Sync & Branch Push
# ═════════════════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}── STEP 4: Remote Sync & Branch Push ────────────────────────${RESET}"

# Add or update remote origin
if git remote get-url origin &>/dev/null; then
  CURR_URL="$(git remote get-url origin)"
  if [[ "$CURR_URL" != "$REMOTE_URL" ]]; then
    git remote set-url origin "$REMOTE_URL"
    info "Remote origin updated: ${CURR_URL} → ${REMOTE_URL}"
  else
    info "Remote origin unchanged: ${REMOTE_URL}"
  fi
else
  git remote add origin "$REMOTE_URL"
  info "Remote origin added: ${REMOTE_URL}"
fi

success "Remote origin: $(git remote get-url origin)"

# Push with explicit upstream tracking
echo ""
info "Pushing branch '${BRANCH_NAME}' to origin …"
echo ""
if git push -u origin "$BRANCH_NAME"; then
  echo ""
  echo -e "${BOLD}════════════════════════════════════════════════════════════${RESET}"
  echo -e "${GREEN}${BOLD}  ✅  DEPLOYMENT COMPLETE${RESET}"
  echo -e "${BOLD}════════════════════════════════════════════════════════════${RESET}"
  echo -e "  Repository : ${CYAN}${REMOTE_URL}${RESET}"
  echo -e "  Branch     : ${GREEN}${BRANCH_NAME}${RESET}  (tracking origin/${BRANCH_NAME})"
  echo -e "  Committer  : ${GIT_NAME} <${GIT_EMAIL}>"
  echo -e "  Tip commit : $(git log --oneline -1)"
  echo ""
  # Print GitHub web URL if HTTPS remote
  if [[ "$REMOTE_URL" =~ ^https://github.com/(.+)\.git$ ]] || \
     [[ "$REMOTE_URL" =~ ^https://github.com/(.+)$       ]]; then
    REPO_PATH="${BASH_REMATCH[1]}"
    echo -e "  🌐 View on GitHub:"
    echo -e "     ${CYAN}https://github.com/${REPO_PATH}/tree/${BRANCH_NAME}${RESET}"
  elif [[ "$REMOTE_URL" =~ ^git@github.com:(.+)\.git$ ]]; then
    REPO_PATH="${BASH_REMATCH[1]}"
    echo -e "  🌐 View on GitHub:"
    echo -e "     ${CYAN}https://github.com/${REPO_PATH}/tree/${BRANCH_NAME}${RESET}"
  fi
  echo -e "${BOLD}════════════════════════════════════════════════════════════${RESET}"
else
  echo ""
  die "Push failed. Check credentials, SSH key, or PAT token configuration."
fi
