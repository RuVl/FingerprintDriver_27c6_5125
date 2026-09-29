<!-- Комментарий в !648 после создания нового MR (подставить номер) -->
I have opened !NNN, which builds on this MR for the same sensor (27c6:5125,
HONOR MagicBook 16). It keeps the ChicagoHS port and the white-box pairing
from here and adds a finger-lift detection that works on this hardware
(FDT mode polling instead of FDT-up), hardening of the state and template
loading, unit tests and a umockdev recording that replays byte for byte.

Thank you @Thomas97460 and @berkekbgz for the groundwork; you are credited
with `Co-authored-by:` in the commits. If you agree, this MR could be closed
in favour of !NNN, so that reviewers only need to look at one of them.
