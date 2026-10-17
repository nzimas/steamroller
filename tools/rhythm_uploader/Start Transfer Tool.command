#!/bin/bash
cd -- "$(dirname -- "$0")" || exit 1
# Finder-launched Terminal sessions can have a different PATH from development shells.
rhythm_python=''
for candidate in /opt/homebrew/bin/python3 /usr/local/bin/python3 "$(command -v python3)" /usr/bin/python3; do
  if [ -x "$candidate" ] && "$candidate" -c 'import http.server, pathlib, subprocess; import sys; assert sys.version_info >= (3, 8)' >/dev/null 2>&1; then
    rhythm_python="$candidate"
    break
  fi
done
if [ -z "$rhythm_python" ]; then
  echo 'Python 3 is needed to serve these local files. Nothing has been installed.'
  read -r -p 'Press Return to close. '
  exit 1
fi
"$rhythm_python" -u serve.py "$@"
rhythm_result=$?
if [ "$rhythm_result" -ne 0 ]; then
  echo
  echo 'The transfer tool could not start. The error is shown above.'
  if [ -t 0 ]; then read -r -p 'Press Return to close. '; fi
fi
exit "$rhythm_result"
